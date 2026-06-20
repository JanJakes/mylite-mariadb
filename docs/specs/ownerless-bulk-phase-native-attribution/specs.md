# Ownerless Bulk Phase Native Attribution

## Problem

The ownerless bulk phase timing slice showed that the first 100-row row-list
statement and the remaining non-empty-table row-list statements have very
different performance profiles. The existing stats-enabled probe still emits
deep InnoDB counters only for the whole bulk loop, so the native row/undo,
commit/history, and default-checked bulk-start buckets cannot be assigned to
the first statement or the later statements without manual inference.

The most obvious optimization, broadening MariaDB's empty-table
`TRX_UNDO_EMPTY` bulk path to non-empty tables, is not currently justified.
MariaDB source ties the DML bulk marker and rollback model to the first
statement that inserts into an empty table.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc::row_ins_clust_index_entry_low()`
  gates DML bulk buffering on `page_is_empty(block->page.frame)` for the
  clustered root page before writing `TRX_UNDO_EMPTY`.
- `mariadb/storage/innobase/include/trx0trx.h::trx_mod_table_time_t::BULK`
  documents that `TRX_UNDO_EMPTY` covers subsequent operations for "the first
  statement to insert into an empty table".
- `mariadb/storage/innobase/include/trx0trx.h::trx_t::end_bulk_insert()`
  documents that later operations require row-level undo so `ROLLBACK TO
  SAVEPOINT` or statement rollback can work.
- `packages/libmylite/tests/embedded_performance_probe.c` already captures
  total ordinary and ownerless bulk deep InnoDB counters, and now times the
  first and remaining bulk statements separately.

## Design

When deep InnoDB counters are enabled for the embedded performance probe, take
one snapshot immediately after the first bulk statement in the ordinary and
ownerless bulk loops. After the existing total counters are read, subtract the
first snapshot from the total counters to derive a remaining-statements
snapshot.

Emit the same compact deep-InnoDB comparison rows that the aggregate bulk
summary already emits, with `first_` and `remaining_` metric prefixes. This
keeps the output stable and comparable:

- aggregate rows continue to use existing names;
- first-statement rows show the empty-table/default-checked path;
- remaining-statement rows show the non-empty-table path.

No production SQL path, InnoDB behavior, ownerless lock protocol, WAL format,
checkpoint policy, or visibility behavior changes.

## Compatibility Impact

None. This is diagnostic-only performance attribution in the embedded
performance probe. It does not broaden SQL-level bulk insert coverage.

## Directory And Lifecycle Impact

None. The probe still creates and removes its temporary MyLite database
directory through the existing performance harness.

## Native Storage Impact

None. Native InnoDB insert, undo, redo, and rollback behavior is unchanged.

## Test And Verification Plan

- Build `mylite_embedded_performance_probe` with the production PHP embedded
  preset.
- Run a reduced stats-enabled 500-row, 100-row-per-statement production probe
  and confirm aggregate, `first_`, and `remaining_` deep comparison rows are
  emitted.
- Run a reduced stats-off production probe to confirm normal throughput output
  still works without attribution enabled.
- Run production build guards, focused ownerless primitives, format, tidy, and
  `git diff --check`.

## Acceptance Criteria

- Aggregate bulk deep comparison rows keep their current names.
- First-statement deep comparison rows are emitted for the same key commit,
  row insert, clustered B-tree, undo report, and default-checked bulk-start
  buckets.
- Remaining-statement deep comparison rows are emitted for the same buckets.
- One-statement probes report zero remaining-statement averages rather than
  failing or reusing aggregate counters.
- Docs continue to avoid claiming broad non-empty-table bulk optimization.

## Verification Results

Passed:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`
- Reduced stats-enabled 500-row production attribution probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=100
MYLITE_PERF_INSERT_ITERATIONS=500
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The 500-row probe emitted aggregate, `first_`, and `remaining_` deep rows. The
phase rows showed the expected split:

- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_first_row_insert_ms_per_statement=-0.739`
- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_first_trx_undo_report_ms_per_statement=-0.334`
- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_first_innodb_default_checked_bulk_starts_per_statement=1.000`
- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_remaining_row_insert_ms_per_statement=2.104`
- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_remaining_trx_undo_report_ms_per_statement=1.082`
- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_remaining_trx_undo_report_mtr_commit_ms_per_statement=0.829`
- `mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_remaining_innodb_default_checked_bulk_starts_per_statement=0.000`

- Reduced stats-enabled one-statement probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=100
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The one-statement probe emitted zero remaining-statement averages, including
`mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_remaining_row_insert_ms_per_statement=0.000`
and
`mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_remaining_trx_undo_report_ms_per_statement=0.000`.

- Reduced stats-off 500-row production probe passed with the existing
  throughput summaries still present.
- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure`
- `git diff --check`
