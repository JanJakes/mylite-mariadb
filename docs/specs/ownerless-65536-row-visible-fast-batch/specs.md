# Ownerless 65536 Row Visible-Fast Batch

## Problem

Ownerless visible-fast append batching currently admits parser-proven pure
`INSERT ... VALUES` row lists through `32768` row constructors. The previous
slice proved the `32768` boundary and left row lists above that edge on the
conservative append-session policy.

The next bounded performance slice is to apply the same policy to the adjacent
`65536` row-list edge without changing native redo/checkpoint semantics,
history-proof publication, native undo handling, or broader DML admission.

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

- Raise the pure row-list append-batch cap from `32768` to `65536`.
- Move focused SQL coverage from the `32768`/`32769` boundary to the
  `65536`/`65537` boundary.
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
- Run a reduced production performance probe with
  `MYLITE_PERF_INSERT_ITERATIONS=131072` and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=65536`.
- Run production build guards, formatting, and whitespace checks.

## Acceptance Criteria

- A `65536` row pure ownerless insert uses one append session and enables
  deferred latest-only checkpoint coalescing.
- A `65537` row pure ownerless insert stays outside append batching and
  deferred latest-checkpoint coalescing while preserving visible-fast commit
  publication.
- Performance evidence shows the new boundary keeps the measured huge
  row-list shape on the append-batched path, and documents remaining native
  single-row and later-statement costs rather than hiding them.
- Broader native row-level undo/MTR cost, redo/checkpoint reconciliation, and
  row lists above `65536` remain separate work.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
  passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path` passed, proving the
  `65536` positive and `65537` conservative SQL boundaries.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --output-on-failure` passed.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(ownerless-single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|embedded-ownerless-innodb-lock-hooks)$'
  --output-on-failure` passed.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` passed.
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|cross-process-checksum-stress)$'
  --output-on-failure` passed.
- A reduced production probe with `MYLITE_PERF_INSERT_ITERATIONS=131072` and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=65536` reported ownerless
  two-statement bulk throughput at `87019.79` rows/s against ordinary
  `157447.10` rows/s (`0.5527` ratio). The first `65536`-row statement was
  close to ordinary at `148537.52` rows/s against `151011.68` rows/s
  (`0.9836` ratio), while the remaining non-empty-table statement reported
  `62337.71` rows/s against ordinary `171418.80` rows/s (`0.3637` ratio).
  The same run kept active-runtime reconnect close to ordinary
  (`0.959 ms` ownerless versus `0.915 ms` ordinary), but exposed the larger
  remaining write-performance gap in single-row ownerless autocommit
  (`435.56 ops/s` versus ordinary `3420.87 ops/s`, `0.1273` ratio).
- `tools/check-ci-production-builds`, `ctest --preset prod -R
  '^tools\.ci-production-builds$' --output-on-failure`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `cmake --build --preset format-check-prod`, and `git diff --check` passed.

## Risks

The larger cap can hold the page-log append session for longer single-owner
bulk statements. The risk is bounded by the explicit `65536` row limit, the
single-owner proof for transaction-deferred publication, and the adjacent
`65537` conservative test. This slice does not address the measured single-row
autocommit gap or the later non-empty-table bulk gap.
