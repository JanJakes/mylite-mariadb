# Ownerless Handler Autocommit Profile

## Problem

The production MTR page-publish profile shows ownerless page publication is a
real cost, but it does not explain the whole one-row autocommit insert gap. A
400-row production sample reported ownerless
`prepared_step_mysql_execute_ms=1513.851` while ownerless MTR commit-log work
was `312.267ms`.

The next profiling boundary is the SQL handler and InnoDB handler layer around
`mysql_stmt_execute()`: single-row `INSERT` execution may spend time in
`ha_innobase::write_row()`, `external_lock()` statement-end autocommit,
`innobase_commit()`, or MariaDB's `ha_commit_trans()` / one-phase engine
commit loop before it reaches the already-measured MTR and ownerless
page-visible hooks.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  `ha_innobase::write_row()` handles auto-increment, template preparation,
  `row_insert_for_mysql()`, error conversion, and WSREP key append hooks.
- `ha_innobase::external_lock()` marks statement boundaries. On `F_UNLCK`, if
  the transaction is autocommit and started, it calls `innobase_commit(thd,
  TRUE)`.
- `innobase_commit()` handles the InnoDB handlerton commit callback. For
  autocommit statement end it runs ordered commit if needed, wakes subsequent
  commits, calls `trx_commit_complete_for_mysql()`, deregisters the InnoDB
  transaction from MariaDB two-phase commit state, and resets statement-level
  InnoDB state.
- `innobase_commit_low()` wraps `trx_commit_for_mysql()`; lower MTR and
  ownerless page-visible phases are already measured by the existing
  commit-visibility and MTR page-publish stats.
- `mariadb/sql/handler.cc` `ha_commit_trans()`, `ha_commit_one_phase()`, and
  `commit_one_phase_2()` are the MariaDB SQL-layer transaction commit
  plumbing that call each storage engine's `commit` hook.

## Design

Add two opt-in production diagnostics families used only by the embedded
performance probe:

- SQL handler commit stats in `handler.cc`:
  `ha_commit_trans()`, `ha_commit_one_phase()`, `commit_one_phase_2()`, engine
  commit-hook time inside `commit_one_phase_2()`, and transaction cleanup time.
- InnoDB handler stats in `ha_innodb.cc`:
  `start_stmt()`, `external_lock()`, statement-end autocommit commit time from
  `external_lock(F_UNLCK)`, `write_row()` total plus auto-increment/template/
  row-insert/post-insert phases, `innobase_commit()`,
  `innobase_commit_ordered_2()`, `innobase_commit_low()`, and the
  `trx_commit_complete_for_mysql()` call inside `innobase_commit()`.

Both families use process-local relaxed atomics behind explicit enable flags.
The performance probe enables, resets, reads, and prints them only when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, alongside the existing ownerless
publish, commit-visibility, MTR, page-log, and database stats.

## Compatibility Impact

No SQL behavior, public C API, PHP API, native storage format, locking,
checkpoint, recovery, or directory-layout changes. The new symbols are
internal diagnostics for first-party probes.

## Directory And Lifecycle Impact

No durable files or directory-layout changes. The counters are process-local
and reset between the transactional and autocommit ownerless insert phases.

## Native Storage Impact

No native InnoDB storage behavior changes. The slice only times existing
handler and transaction commit paths.

## Build And Performance Impact

Stats-off paths keep their normal behavior. The added broad SQL/handler probes
must keep the cold path cheap by checking the explicit stats flag before
reading clocks or touching atomics. Stats-on samples are diagnostic and should
not be treated as exact non-probe throughput.

## Test Plan

- Rebuild the MariaDB embedded archive after editing MariaDB source files.
- Rebuild production embedded targets for the performance probe and focused
  ownerless SQL/primitive tests.
- Run a reduced stats-enabled production performance probe and confirm the new
  SQL handler and InnoDB handler fields are emitted for ownerless insert phases.
- Run focused ownerless primitive and SQL selectors covering visibility,
  native reclaim, live reclaim, commit race, and active-reader pressure.
- Run production unsafe-hook negative proof, `format-check-prod`, and
  `git diff --check`.

## Results

Reduced production probe command:

```sh
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 \
MYLITE_PERF_SELECT_ITERATIONS=100 \
MYLITE_PERF_INSERT_ITERATIONS=400 \
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1 \
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The 400-row ownerless autocommit insert sample reported:

- `mylite_perf_ownerless_insert_autocommit_ops_per_second=234.65`
- `prepared_step_mysql_execute_ms=1394.479`
- `sql_handler_ha_commit_trans_total_ms=870.340`
- `sql_handler_commit_one_phase_2_engine_commit_ms=858.716`
- `innodb_handler_innodb_commit_total_ms=858.069`
- `innodb_handler_innodb_commit_low_total_ms=856.356`
- `innodb_handler_write_row_total_ms=297.606`
- `innodb_handler_write_row_insert_ms=297.183`
- `page_write_commit_log_total_ms=248.803`
- `page_write_commit_log_publish_ms=157.807`
- `page_publish_hook_total_ms=133.294`
- `page_log_append_total_ms=110.140`

The matching ownerless transactional insert sample reported
`1476.45 ops/s`, `prepared_step_mysql_execute_ms=139.914`,
`innodb_handler_write_row_total_ms=41.459`, and one measured
`innodb_commit_low_total_ms=32.953` commit, showing that the high autocommit
cost is per-statement commit work rather than ordinary row insertion alone.

The missing ownerless autocommit time therefore sits primarily below
MariaDB's one-phase SQL commit path in InnoDB commit execution, with
`innobase_commit_low()`/`trx_commit_for_mysql()` the dominant measured
boundary. `row_insert_for_mysql()` is the next largest measured block. Page
publication remains material, but it is a subpart of the InnoDB commit/row
execution cost rather than the whole `mysql_stmt_execute()` gap.

## Acceptance Criteria

- The embedded performance probe emits SQL handler and InnoDB handler timing
  fields when ownerless page-publish stats are enabled.
- Existing focused ownerless correctness coverage passes.
- The spec records whether the missing `mysql_stmt_execute()` time sits in
  row insert, handler commit, SQL commit plumbing, or remains above these
  boundaries.

## Risks And Follow-Up

- More diagnostics can perturb stats-on timings; use them for attribution, not
  absolute throughput.
- The next performance slice should profile `trx_commit_for_mysql()` and the
  ownerless redo/page-publication work it triggers from inside
  `innobase_commit_low()`.
- A parallel or follow-up slice should profile `row_insert_for_mysql()`, since
  row-insert internals are the next largest measured block in the ownerless
  autocommit sample.
