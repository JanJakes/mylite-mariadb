# CI Ownerless SQL Weighted Shards

## Problem Statement

The repository already registers ownerless cross-process SQL coverage as
sixteen deterministic weighted CTest shards. The CI `ubuntu-embedded` job was
still bypassing that registration and running every ownerless SQL case through
a serial shell loop. That made the step slower, hid the per-shard CTest timing
data, and diverged from the measured shard evidence in the ownerless SQL
scheduling specs.

MyLite needs the CI timing path to use the same production CTest shard graph it
is trying to measure.

## Source Findings

- `.github/workflows/ci.yml` built production embedded artifacts, then ran
  `mylite_ownerless_cross_process_sql_test sql-case <index>` serially for each
  reported case count.
- `packages/libmylite/CMakeLists.txt` registers
  `libmylite.ownerless-cross-process-sql.<n>` with
  `sql-weighted-shard <index> 16` under the
  `compat.ownerless-cross-process-sql` label.
- `docs/specs/ownerless-sql-smaller-weighted-shards/specs.md` records a
  passing CI-shaped two-job run of the registered sixteen-shard group.
- `tools/check-ci-production-builds` already guards CI timing steps for
  production CMake caches, but still required the old serial direct-case
  markers.

## Design

Keep the production build guards in the CI ownerless SQL step, then run the
registered production CTest shard graph directly:

```text
ctest --preset php-embedded-prod \
  -R '^libmylite\.ownerless-cross-process-sql\.' \
  --parallel 2 \
  --output-on-failure
```

The step writes the shard selector and parallelism to the GitHub step summary.
CTest provides per-shard timings, and the existing ownerless SQL child wrapper
still emits active case names, case indexes, and process diagnostics on
timeout.

Update the CI production-build guard to require the production CTest shard
path, the ownerless SQL selector, and the two-job parallelism so future CI
edits cannot silently fall back to a debug build or the old monolithic loop.

## Scope

In scope:

- GitHub Actions ownerless SQL test scheduling.
- CI production-build guard updates.
- Compatibility and slice documentation.

Out of scope:

- Product ownerless SQL, storage, locking, page-version, redo, or recovery
  behavior.
- Changing the registered shard count or weighted-assignment algorithm.
- Increasing ownerless SQL per-case timeouts.
- Broader CI cancellation policy.

## Compatibility Impact

No MySQL/MariaDB compatibility behavior changes. This changes only CI
scheduling and timing visibility for already-registered regression tests.

## Database Directory And Lifecycle Impact

No database-directory layout or lifecycle changes. Each ownerless SQL case
continues to create and remove its own temporary MyLite database directory
through the existing child-runner helpers.

## Native Storage Impact

No native storage format or runtime behavior changes.

## Public API, Build, Size, And Dependencies

No public API, production build-profile, binary-size, license, or dependency
changes. CI continues to require `MinSizeRel` MariaDB embedded artifacts and
`Release` MyLite production artifacts before publishing ownerless SQL timings.

## Test Plan

- Run `tools/check-ci-production-builds`.
- Validate production CTest discovery for the ownerless SQL shard selector.
- Run a focused production ownerless SQL weighted-shard smoke.
- Run the exact production ownerless SQL shard command that CI will use.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- CI ownerless SQL uses `ctest --preset php-embedded-prod` instead of the
  direct serial `sql-case` loop.
- The CI selector targets the registered
  `libmylite.ownerless-cross-process-sql.<n>` weighted shards.
- The step keeps production build guards before timing output.
- `tools/check-ci-production-builds` rejects workflows that remove the
  production CTest shard path or its `--parallel 2` scheduling, while mixed
  embedded CTest steps remain serial.

## Evidence

The pre-change production CI run at `88352856` passed, but the
`ubuntu-embedded` ownerless SQL step used the direct serial loop:

```text
ownerless_sql_case_count=180
ownerless_sql_case_start index=0
ownerless_sql_case_seconds index=179 status=0 seconds=1
Run embedded ownerless SQL tests: 2026-06-18T22:00:12Z..2026-06-18T22:03:56Z
```

The production-build audit passed after the workflow and guard update:

```text
tools/check-ci-production-builds
ci_production_build_audit_ok=.github/workflows/ci.yml
```

Production CTest discovery reports the registered sixteen weighted ownerless
SQL shards:

```text
ctest --preset php-embedded-prod -N -R '^libmylite\.ownerless-cross-process-sql\.'
Total Tests: 16
```

A focused production CTest smoke of shard 0 passed:

```text
ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.0$' --output-on-failure
100% tests passed, 0 tests failed out of 1
Total Test time (real) = 32.31 sec
```

The exact production command used by CI passed locally and exposed per-shard
timings:

```text
ctest --preset php-embedded-prod -R '^libmylite\.ownerless-cross-process-sql\.' --parallel 2 --output-on-failure
100% tests passed, 0 tests failed out of 16
Total Test time (real) = 164.07 sec
```

The first CI run after this change should still be used as the runner-level
wall-time comparison for the production weighted-shard path.
