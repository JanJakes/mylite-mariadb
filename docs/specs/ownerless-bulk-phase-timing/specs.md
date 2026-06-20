# Ownerless Bulk Phase Timing

## Problem

The production embedded performance probe reports aggregate bulk insert
throughput for ordinary and ownerless `INSERT ... VALUES` statements. That
aggregate hides two materially different InnoDB paths in the current ownerless
profile:

- the first row-list statement into an empty primary-key-only table can use the
  bounded ownerless default-checked empty-table bulk path;
- later row-list statements insert into a non-empty table and fall back to
  ordinary per-row clustered insert, undo-report, mini-transaction, and
  ownerless page-write work.

The aggregate row/statement rate is still useful, but it is not enough to
decide whether a branch/main change moved startup cost, the first empty-table
bulk path, or steady non-empty-table insert execution.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0ins.cc::row_ins_clust_index_entry_low()`
  admits MariaDB's empty-table bulk insert path only while the clustered root
  page is empty and the other bulk gates hold.
- `docs/specs/ownerless-default-checked-bulk-insert/specs.md` intentionally
  limits the MyLite ownerless default-checked relaxation to that empty-table
  primary-key-only shape.
- `packages/libmylite/tests/embedded_performance_probe.c` builds one table per
  bulk phase, times the whole row-list loop, and emits aggregate ordinary and
  ownerless bulk rates. Before this slice it did not separately time the first
  `mylite_exec()` call and the remaining calls.

## Design

Extend `measure_bulk_autocommit_insert()` with optional phase timing for:

- first bulk statement rows and statement count;
- remaining bulk statement rows and statement count.

The timing is scoped around the `exec_ok()`/`mylite_exec()` call for each
row-list statement, so it measures SQL execution rather than SQL string
construction. The probe continues to emit the existing aggregate bulk rows and
statement rates, and now adds raw and compact summary rows for the first
statement and remaining statements. Both ordinary and ownerless phases use the
same timing code, so ownerless/ordinary ratios remain directly comparable.

No production SQL path, InnoDB path, page-version WAL format, lock protocol,
checkpoint policy, or ownerless visibility behavior changes.

## Compatibility Impact

None. This is diagnostic-only performance attribution in the embedded
performance probe.

## Directory And Lifecycle Impact

None. The probe still creates and removes its temporary MyLite database
directory through the existing performance harness.

## Native Storage Impact

None. Native InnoDB bulk and non-bulk insert behavior is unchanged.

## Test And Verification Plan

- Build `mylite_embedded_performance_probe` with the production PHP embedded
  preset.
- Run a reduced stats-off production probe with 500 rows and 100 rows per bulk
  statement, confirming first-statement and remaining-statement rows appear for
  both ordinary and ownerless phases.
- Run a reduced stats-enabled attribution probe to ensure the added timing does
  not hide existing ownerless page-log, page-write, commit-visibility, SQL
  handler, InnoDB handler, and deep InnoDB summaries.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Existing aggregate bulk probe rows remain present.
- First-statement and remaining-statement raw rows are emitted for ordinary and
  ownerless bulk phases.
- Compact summaries include ownerless/ordinary ratios for first-statement rows,
  first-statement statements, remaining rows, and remaining statements.
- The docs continue to state that broader non-empty-table bulk optimization is
  planned rather than implemented.

## Verification Results

Passed:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`
- Reduced stats-off production probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=100
MYLITE_PERF_INSERT_ITERATIONS=500
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The stats-off probe emitted the new first/remaining timing rows. Representative
summary rows:

- `mylite_perf_summary_ownerless_insert_autocommit_bulk_first_statement_rows_ratio=0.7335`
- `mylite_perf_summary_ownerless_insert_autocommit_bulk_remaining_rows_ratio=0.2836`
- `mylite_perf_summary_ownerless_insert_autocommit_bulk_first_statement_ratio=0.7335`
- `mylite_perf_summary_ownerless_insert_autocommit_bulk_remaining_statements_ratio=0.2836`

This confirms the first empty-table 100-row statement and the later
non-empty-table statements are now visible as separate timing buckets.

- Reduced stats-enabled attribution probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=100
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The stats-enabled one-statement probe preserved existing ownerless attribution
rows, including
`mylite_perf_summary_ownerless_autocommit_bulk_page_versions_per_statement=2.000`,
`mylite_perf_summary_ownerless_autocommit_bulk_default_checked_bulk_starts_per_statement=1.000`,
and
`mylite_perf_summary_ownerless_minus_ordinary_autocommit_bulk_innodb_default_checked_bulk_starts_per_statement=1.000`.

Additional local checks passed:

- `tools/check-ci-production-builds`
- `cmake --build --preset format-check-prod`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure`
- `git diff --check`
