# Ownerless 16384 Row Visible-Fast Batch

## Problem

Ownerless visible-fast append batching currently admits parser-proven pure
`INSERT ... VALUES` row lists through `8192` row constructors. The previous
slice proved that the `8192` boundary collapses append-session churn and
snapshot-boundary publication for two-statement bulk probes, while keeping
`8193` rows conservative. The next bounded performance slice is to apply the
same policy to the adjacent `16384` row-list edge without changing the broader
redo/checkpoint contract.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `ownerless_insert_values_statement_row_count()` scans the original SQL text
  and returns a count only for plain row-constructor lists.
- `packages/libmylite/src/database.cc`
  `ownerless_statement_fast_path_policy()` keeps visible-fast commit
  publication separate from append-batch admission. Row lists above the cap may
  still use visible-fast commit publication, but do not hold one page-log
  append session or coalesce latest-only checkpoint updates across the
  statement.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `test_ownerless_single_owner_multi_row_insert_visible_fast_path()` owns the
  focused positive and adjacent conservative boundary checks.

## Design

- Raise the pure row-list append-batch cap from `8192` to `16384`.
- Move focused SQL coverage from the `8192`/`8193` boundary to the
  `16384`/`16385` boundary.
- Keep all other admission checks unchanged: single-owner epoch is still
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
This slice only changes internal ownerless batching policy for a
parser-proven, single-owner, foreign-key-free row-list shape.

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
  `MYLITE_PERF_INSERT_ITERATIONS=32768` and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=16384`.
- Run production build guards, formatting, and whitespace checks.

## Acceptance Criteria

- A `16384` row pure ownerless insert uses one append session and enables
  deferred latest-only checkpoint coalescing.
- A `16385` row pure ownerless insert stays outside append batching and
  deferred latest-checkpoint coalescing while preserving visible-fast commit
  publication.
- Performance evidence shows the new boundary reduces append-session/page-log
  churn for the measured large-row shape, or documents why the larger bound did
  not help.
- Broader native row-level undo/MTR cost, redo/checkpoint reconciliation, and
  still larger row-list admission remain separate work.

## Verification Results

Passed:

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --output-on-failure`
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(ownerless-single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|embedded-ownerless-innodb-lock-hooks)$'
  --output-on-failure`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|cross-process-checksum-stress)$'
  --output-on-failure`
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The reduced production probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=32768
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=16384
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

It passed and reported two bulk statements with `16384` rows per statement:

- page-log append-session begin/end calls: `2`/`2`;
- page-log append-session append calls: `90`;
- snapshot-boundary publications: `0`;
- page versions: `2.000` per statement;
- page-log append calls: `45.000` per statement;
- native-support published pages: `2.000` per statement;
- visible-fast commits: `1.000` per statement;
- visibility flushes: `0.000` per statement;
- deferred latest-checkpoint coalesces: `513.000` per statement;
- ownerless `mysql_query()`: `220.533 ms/statement`;
- ownerless bulk throughput: `72415.68 rows/s`;
- ownerless/ordinary bulk rows ratio: `0.6971`;
- first-statement ownerless/ordinary rows ratio: `1.7983`;
- later-statement ownerless/ordinary rows ratio: `0.4032`;
- remaining ownerless later-statement row-insert time:
  `301.249 ms/statement`;
- remaining ownerless later-statement undo-report MTR commit time:
  `74.959 ms/statement`.

The focused selector also proved that the adjacent `16385` row statement stays
outside append batching and deferred latest-checkpoint coalescing while keeping
visible-fast commit publication.

## Risks

The larger cap can hold the page-log append session for longer single-owner
bulk statements. The risk is bounded by the explicit `16384` row limit, the
single-owner proof for transaction-deferred publication, and the adjacent
`16385` conservative test.
