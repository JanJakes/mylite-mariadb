# Ownerless SQL CI Serial Shards

## Problem

CI run `27947457764` for `b11c5df34` proved the WordPress block/REST shard
grouping, but the unrelated embedded ownerless SQL step failed twice under
`ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.'
--parallel 2 --output-on-failure`.

The first attempt timed out shard `.8`, case `105`
(`test_ownerless_instant_column_variants_refresh_peer_dictionary`), after the
300-second per-case watchdog. A failed-job rerun then timed out shard `.1`,
case `43` (`test_ownerless_single_owner_multi_row_insert_visible_fast_path`),
also at the 300-second per-case watchdog. Local targeted verification in the
same worktree passed both the direct case and the full shard `.1`.

That evidence points to GitHub Actions load-sensitive interaction between
ownerless SQL shard processes, not a deterministic failure in the WordPress CI
performance changes.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice does not modify
  MariaDB source.
- `packages/libmylite/CMakeLists.txt` registers sixteen deterministic weighted
  ownerless SQL shards, each with a 900-second CTest timeout and a per-case
  child watchdog inside the test binary.
- Shards `.8` and `.11` already have `RUN_SERIAL TRUE` after earlier
  load-sensitive CI timeout evidence, but the failed rerun timed out shard `.1`
  instead.
- `.github/workflows/ci.yml` already separates non-ownerless embedded tests
  from ownerless SQL tests; only the ownerless SQL step still used CTest
  `--parallel 2`.
- The failed cases passed locally when run directly or as isolated shard `.1`,
  so the smallest CI correction is to remove cross-shard concurrency pressure
  from the production ownerless SQL evidence step.

## Design

Run the embedded ownerless SQL CI step with `--parallel 1` while preserving:

- the production `php-embedded-prod` preset;
- the same `^libmylite\.ownerless-cross-process-sql\.` selector;
- the sixteen weighted CTest shard registrations;
- per-case child watchdog diagnostics;
- visible GitHub step summary reporting for selector and parallelism.

The production-build audit now requires the serial ownerless SQL command so the
workflow cannot silently return to the observed load-sensitive `--parallel 2`
shape.

## Non-Goals

- Changing ownerless SQL behavior, test coverage, shard assignment, per-case
  watchdog limits, or MariaDB native storage behavior.
- Claiming that ownerless cross-process implementation is complete.
- Optimizing ownerless SQL CI wall time in this slice.
- Changing WordPress PHPUnit timings or shard grouping.

## Compatibility Impact

No SQL, C API, PHP API, mysqli API, native storage, directory-layout, or
ownerless runtime behavior changes. This changes only CI scheduling for the
existing production ownerless SQL evidence step.

## Build And Performance Impact

Ownerless SQL CI wall time can increase because the sixteen weighted shards no
longer run two at a time. The tradeoff is fewer expensive red CI runs from
load-sensitive cross-shard timeouts. WordPress PHPUnit timing and production
build timing remain unchanged by this slice.

## Test And Verification Plan

- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run the exact direct case that timed out on the first CI attempt.
- Run the exact direct case that timed out on the rerun.
- Run isolated shard `.1` locally.
- Run focused production CTest coverage for the CI production-build audit.
- Run the production format check.
- Run `git diff --check`.
- Let the next pushed CI run provide full production ownerless SQL evidence.

## Acceptance Criteria

- The workflow ownerless SQL step reports and runs with `--parallel 1`.
- The production-build audit requires the serial ownerless SQL command.
- Compatibility docs describe the current serial production evidence.
- Direct local reruns of cases `43` and `105` pass.
- Isolated local shard `.1` passes.

## Verification Results

- `bash -n tools/check-ci-production-builds`: passed.
- `tools/check-ci-production-builds`: passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  --ownerless-sql-test-case=43`: passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  --ownerless-sql-test-case=105`: passed.
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.1$'
  --output-on-failure`: passed.
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.'
  --parallel 1 --output-on-failure`: passed all 16 shards in `372.88s`.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`: passed.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

## Risks And Follow-Up

- Serial ownerless SQL CI reduces flakiness at the cost of wall time. A future
  slice can restore parallelism only after proving cross-shard resource
  isolation for the load-sensitive cases seen in GitHub Actions.
- Because this is a CI scheduling change, it does not itself close remaining
  ownerless correctness gaps such as broader DDL/file lifecycle recovery or
  external MariaDB/RQG stress.
