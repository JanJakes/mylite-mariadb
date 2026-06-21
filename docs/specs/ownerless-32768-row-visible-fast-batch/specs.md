# Ownerless 32768 Row Visible-Fast Batch

## Problem

Ownerless visible-fast append batching currently admits parser-proven pure
`INSERT ... VALUES` row lists through `16384` row constructors. The previous
slice proved that the `16384` boundary collapses append-session churn,
snapshot-boundary publication, and repeated latest-checkpoint updates for a
two-statement production probe while keeping `16385` rows conservative.

The next bounded performance slice is to apply the same policy to the adjacent
`32768` row-list edge without changing native redo/checkpoint semantics,
history-proof publication, or broader DML admission.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `ownerless_insert_values_statement_row_count()` scans the original SQL text
  and returns a row-constructor count only for plain `INSERT ... VALUES` row
  lists.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_fast_path_policy()` keeps visible-fast commit
  publication separate from append-batch admission. Row lists above the cap can
  still use visible-fast commit publication, but they do not hold one page-log
  append session or coalesce latest-only checkpoint updates across the
  statement.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_single_owner_multi_row_insert_visible_fast_path()` owns the
  focused positive and adjacent conservative boundary checks.

## Design

- Raise the pure row-list append-batch cap from `16384` to `32768`.
- Move focused SQL coverage from the `16384`/`16385` boundary to the
  `32768`/`32769` boundary.
- Keep all other admission checks unchanged: single-owner epoch remains
  required for transaction-deferred page publication, target foreign-key tables
  remain conservative, peer-present auto-increment target-column-list shapes
  remain conservative, and broader DML/DDL stays outside this path.

## Non-Goals

- Do not infer unbounded row-list admission from this larger bounded edge.
- Do not change page-version WAL format, native-support proof records,
  redo/checkpoint ordering, native undo records, or recovery behavior.
- Do not change SQL-visible results, duplicate-key handling, PHP/mysqli
  behavior, or public C API behavior.
- Do not claim ownerless concurrency is complete.

## Compatibility Impact

SQL-visible behavior is unchanged. Successful statements insert the same rows
and use the same MariaDB execution path for row insertion and error handling.
This slice only changes internal ownerless batching policy for a parser-proven,
single-owner, foreign-key-free row-list shape.

## Directory, Lifecycle, And Native Storage Impact

No new files, shared-memory fields, directory layout, native InnoDB page
format, redo format, undo format, checkpoint format, startup behavior, or close
behavior change. Durable state remains inside the MyLite database directory
through native InnoDB files and the existing ownerless page-version WAL.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run `single-owner-multi-row-insert-visible-fast-path` directly.
- Run adjacent ownerless selectors for history WAL proof, native-support page
  WAL elision, FK fast-path cache, and uncommitted-peer hiding.
- Run a reduced stats-enabled production performance probe with
  `MYLITE_PERF_INSERT_ITERATIONS=65536` and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=32768`.
- Run production build guards, formatting, and whitespace checks.

## Acceptance Criteria

- A `32768` row pure ownerless insert uses one append session and enables
  deferred latest-only checkpoint coalescing.
- A `32769` row pure ownerless insert stays outside append batching and
  deferred latest-checkpoint coalescing while preserving visible-fast commit
  publication.
- Performance evidence shows the new boundary reduces append-session churn for
  the measured large-row shape, and documents the remaining native/page-log
  work at this larger bound.
- Broader native row-level undo/MTR cost, redo/checkpoint reconciliation, and
  row lists above `32768` remain separate work.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path` passed, proving the
  `32768` positive and `32769` conservative SQL boundaries.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --output-on-failure` passed.
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(ownerless-single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|embedded-ownerless-innodb-lock-hooks)$'
  --output-on-failure` passed.
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|cross-process-checksum-stress)$'
  --output-on-failure` passed.
- A reduced stats-enabled production probe with
  `MYLITE_PERF_INSERT_ITERATIONS=65536` and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=32768` reported `2`
  append-session begin/end calls for two statements, zero snapshot-boundary
  publications, `2.000` page versions per statement, `1598.500` page-log
  appends per statement, `2.000` native-support published pages per statement,
  visible-fast commits at `1.000` per statement, deferred latest-checkpoint
  coalesces at `685.000` per statement, ownerless `mysql_query()` at
  `422.337 ms` per statement, and ownerless bulk throughput at `60797.81`
  rows/s against ordinary `63663.34` rows/s (`0.9550` ratio). The later
  non-empty-table statement still reports `115.835 ms` page-write commit-log
  time, `28.976 ms` redo-leave time, and `153.153 ms` undo-report MTR commit
  time per statement.
- `tools/check-ci-production-builds`, `ctest --preset prod -R
  '^tools\.ci-production-builds$' --output-on-failure`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `cmake --build --preset format-check-prod`, and `git diff --check` passed.

## Risks

The larger cap can hold the page-log append session for longer single-owner
bulk statements. The risk is bounded by the explicit `32768` row limit, the
single-owner proof for transaction-deferred publication, and the adjacent
`32769` conservative test.
