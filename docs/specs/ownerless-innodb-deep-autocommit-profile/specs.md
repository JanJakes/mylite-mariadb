# Ownerless InnoDB Deep Autocommit Profile

## Problem

The SQL/InnoDB handler autocommit profile showed the remaining ownerless
one-row autocommit insert cost is mostly below MariaDB's one-phase commit path
and `ha_innobase::write_row()`. In a 400-row production sample,
`prepared_step_mysql_execute_ms=1394.479`, with
`innodb_handler_innodb_commit_low_total_ms=856.356` and
`innodb_handler_write_row_insert_ms=297.183`.

The next profiling boundary must split `trx_commit_for_mysql()` /
`trx_t::commit()` internals and `row_insert_for_mysql()` internals. Without
that split, optimization would risk targeting page-publication append cost when
the actual production branch cost may be generic InnoDB commit bookkeeping,
ownerless commit-visibility work, row execution graph work, per-index insert
work, or B-tree insertion.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_commit_for_mysql()` switches by transaction state and calls
  `trx->commit()` for active or prepared transactions.
- `trx_t::commit()` clears `dict_operation`, calls `commit_persist()`, and then
  runs `commit_cleanup()`.
- `trx_t::commit_persist()` starts a commit mini-transaction, calls
  `write_serialisation_history()` for logged persistent transactions, then
  calls `commit_in_memory(&mtr)`.
- `trx_t::commit_in_memory()` closes read views, transitions transaction state,
  deregisters read/write transactions, updates modified table metadata, runs
  optional log flush, executes MyLite ownerless commit-visibility publication,
  releases locks, cleans temporary undo, finalizes FTS state, and clears
  deadlock-victim state. Existing ownerless commit-visibility counters split
  only the ownerless sub-blocks, not the surrounding InnoDB commit plumbing.
- `mariadb/storage/innobase/row/row0mysql.cc`
  `row_insert_for_mysql()` validates table state, starts the transaction if
  needed, prepares the insert row, converts MySQL row data to InnoDB row data,
  executes `row_ins_step()`, handles lock waits/retries, updates FTS/statistics
  state, and returns the InnoDB error code.
- `mariadb/storage/innobase/row/row0ins.cc` `row_ins_step()` drives the insert
  execution graph, `row_ins()` iterates indexes, `row_ins_index_entry()`
  dispatches clustered versus secondary index entries,
  `row_ins_clust_index_entry()` and `row_ins_sec_index_entry()` perform
  per-index foreign-key and index insertion work, and their low-level helpers
  call `btr_cur_optimistic_insert()` and sometimes
  `btr_cur_pessimistic_insert()`.

## Design

Add one opt-in diagnostic family, used only by the embedded performance probe,
with counters split across the lower InnoDB commit and row-insert paths:

- transaction commit:
  `trx_commit_for_mysql()`, `trx_t::commit()`, `commit_persist()`,
  `commit_in_memory()`, `commit_cleanup()`, the `trx->commit()` subcall inside
  `trx_commit_for_mysql()`, and the `commit_persist()` / `commit_cleanup()`
  subcalls inside `trx_t::commit()`;
- row insert:
  `row_insert_for_mysql()`, transaction start, prebuilt row setup, MySQL-to-
  InnoDB row conversion, `row_ins_step()`, retry/error handling, post-insert
  FTS/statistics cleanup, `row_ins()`, `row_ins_index_entry_step()`,
  `row_ins_index_entry()`, clustered and secondary index entry functions, and
  the clustered/secondary low-level insertion helpers.

The counters use relaxed process-local atomics behind an explicit enable flag.
Stats-off paths check only the flag before reading clocks or updating atomics.
The performance probe enables, resets, reads, and prints the counters only when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, alongside the existing ownerless
commit, page-write, page-log, handler, and database diagnostics.

## Compatibility Impact

No SQL behavior, public C API, PHP API, storage format, locking behavior,
checkpoint behavior, recovery behavior, or directory layout changes. New C
symbols are first-party internal diagnostics consumed by the performance probe.

## Directory And Lifecycle Impact

No durable or transient file changes. Counters are process-local and reset
between the ownerless transactional and autocommit insert samples.

## Native Storage Impact

No native InnoDB storage behavior changes. The slice times existing InnoDB
transaction and row insertion paths only.

## Build And Performance Impact

The slice touches MariaDB-derived InnoDB files and requires rebuilding the
embedded MariaDB archive. Stats-off production/test behavior must remain cheap:
the normal path should not read clocks or touch counters unless the probe
explicitly enables this diagnostic family. Stats-on results are attribution
evidence, not exact non-probe throughput.

## Test Plan

- Rebuild the MariaDB embedded archive.
- Rebuild production embedded targets for the performance probe and focused
  ownerless SQL/primitive tests.
- Run a reduced stats-enabled production performance probe and confirm the new
  deep InnoDB fields are emitted for transactional and autocommit ownerless
  insert phases.
- Run focused ownerless primitive and SQL selectors covering committed-read
  visibility, native reclaim, live reclaim, commit race, and active-reader
  pressure.
- Run `ownerless-test-hooks` negative proof, `ownerless-stress`,
  `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- The embedded performance probe emits the lower InnoDB commit and row-insert
  timing fields when ownerless page-publish stats are enabled.
- Existing focused ownerless correctness and stress coverage passes.
- The spec records whether the ownerless autocommit cost is dominated by
  `commit_in_memory()` ownerless visibility, generic commit cleanup/persist
  work, row execution graph work, per-index insertion, or B-tree insertion.

## Results

The production embedded archive and `php-embedded-prod` targets were rebuilt
before collecting timings. With stats disabled, the reduced 400-row production
probe stayed in the same range as the prior ownerless autocommit sample:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=100
MYLITE_PERF_INSERT_ITERATIONS=400
mylite_perf_ordinary_insert_autocommit_ops_per_second=1612.34
mylite_perf_ownerless_insert_autocommit_ops_per_second=235.84
```

With `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, the new deep counters split
the ownerless autocommit insert cost as follows for the same 400-row shape:

```text
mylite_perf_ownerless_insert_autocommit_prepared_step_mysql_execute_ms=1210.199
mylite_perf_ownerless_insert_autocommit_innodb_handler_innodb_commit_low_total_ms=778.650
mylite_perf_ownerless_insert_autocommit_innodb_deep_trx_commit_for_mysql_total_ms=778.063
mylite_perf_ownerless_insert_autocommit_innodb_deep_trx_commit_persist_write_history_ms=749.591
mylite_perf_ownerless_insert_autocommit_innodb_deep_trx_commit_persist_in_memory_ms=27.109
mylite_perf_ownerless_insert_autocommit_innodb_deep_trx_commit_in_memory_ownerless_ms=19.878
mylite_perf_ownerless_insert_autocommit_innodb_deep_row_insert_for_mysql_total_ms=229.768
mylite_perf_ownerless_insert_autocommit_innodb_deep_row_insert_step_ms=224.107
mylite_perf_ownerless_insert_autocommit_innodb_deep_row_ins_clust_low_total_ms=218.261
mylite_perf_ownerless_insert_autocommit_innodb_deep_row_ins_btr_optimistic_total_ms=175.924
```

The ownerless autocommit gap is therefore dominated by
`trx_t::write_serialisation_history()` and its commit mini-transaction, not by
the post-commit ownerless visibility block. Existing page-write/page-log stats
show that this write-history bucket includes ownerless commit-MTR page
publication and page-log append work (`page_write_commit_log_publish_ms=170.927`
and `page_log_append_total_ms=126.903` in the same stats-enabled run). The next
optimization slice should target commit-MTR page publication / page-log append
cost first, with clustered B-tree row-insert cost as the next major block.

## Risks And Follow-Up

- More clock reads perturb stats-on timings; use the stats-disabled run for
  production throughput comparison and the stats-enabled run for attribution.
- If commit-MTR publication optimization moves the bottleneck, repeat the deep
  probe before changing native row-insert paths.
- If row-insert cost is dominated by low-level clustered/secondary insertion,
  the next slice should inspect page-write acquisition/publication frequency
  inside insert mini-transactions.
