# Embedded CI Serial Runtime Tests

## Problem

The production embedded CI job ran non-ownerless CTest coverage with
`--parallel 2`. That made the step shorter, but it also allowed independent
test processes to initialize and shut down MariaDB embedded at the same time.
The current branch has enough embedded lifecycle coverage that those concurrent
runtime startups can make CI fail for reasons unrelated to the test under
inspection.

The failure observed on 2026-06-10 happened in the production `ubuntu-embedded`
job before ownerless SQL cases ran. `libmylite.embedded-open-close` failed
against a stale test expectation for the redo-state shared-memory segment
version, and local reproduction also showed that running separate embedded
CTest invocations concurrently can make `libmylite.embedded-same-process-
concurrency` fail during InnoDB startup. The later serial CI-shaped
non-ownerless embedded run passed that same test before continuing through the
rest of the suite.

## Source Findings

- The embedded job already builds production artifacts:
  `build/mariadb-embedded` is `MinSizeRel`, and `build/php-embedded-prod` is
  `Release`.
- The failed CI log for run `27296377202`, job `80630101366`, reported
  `embedded-open-close` asserting that the redo-state segment version was `7`,
  while production code has `k_concurrency_redo_state_segment_version = 8`.
- A local serial rerun of `libmylite.embedded-open-close` passed after updating
  the test expectation to version `8`.
- A local `ctest --preset php-embedded-prod -R
  '^libmylite\.(embedded-open-close|embedded-same-process-concurrency)$'
  --parallel 2` launched independent embedded runtimes concurrently and failed
  the old open-close binary. A separate accidental concurrent pair of CTest
  invocations reproduced the InnoDB startup failure in
  `embedded-same-process-concurrency`. The later serial non-ownerless embedded
  suite passed `embedded-same-process-concurrency`.

## Design

Keep the production build guards and the ownerless SQL direct-case loop
unchanged. Remove `--parallel 2` from the embedded non-ownerless CTest step so
the job runs those MariaDB embedded runtime tests serially.

Update `tools/check-ci-production-builds` to reject `ctest ... --parallel N`
inside the workflow. Parallelism remains available to local developers when
they knowingly want a stress run, but CI timing and compatibility evidence
should not depend on concurrent `libmysqld` process startup.

Update the open-close test's shared-memory layout expectation for the
redo-state segment version from `7` to `8`, matching
`packages/libmylite/src/database.cc`.

## Compatibility Impact

No SQL, public API, mysqli, storage-engine, or directory-lifecycle behavior
changes. This changes CI scheduling and a test expectation only.

## Build And Performance Impact

No production binary impact. The embedded CI job may take longer because
non-ownerless embedded CTests now run serially, but the timing is more useful:
it measures one production embedded runtime at a time instead of a race between
two independent MariaDB embedded startup/shutdown cycles.

## Test And Verification Plan

- Rebuild `mylite_embedded_open_close_test` with `php-embedded-prod`.
- Run `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-open-close$' --output-on-failure`.
- Run the CI-shaped guarded non-ownerless embedded command without
  `--parallel`:
  `ctest --preset php-embedded-prod -LE compat.ownerless-cross-process-sql
  --output-on-failure`.
- Run `bash -n tools/check-ci-production-builds`.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-10 used production embedded build caches:
`build/mariadb-embedded` was `MinSizeRel`, and `build/php-embedded-prod` was
`Release`.

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_open_close_test`: passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-open-close$' --output-on-failure`: passed, 1/1 tests,
  `16.56 sec`.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` and
  `tools/require-cmake-release-build build/php-embedded-prod`: passed.
- The serial CI-shaped non-ownerless embedded command passed:
  `ctest --preset php-embedded-prod -LE compat.ownerless-cross-process-sql
  --output-on-failure`, 53/53 tests, `127.92 sec`.
- A local contaminated/concurrent check reproduced the underlying instability:
  concurrent embedded CTest processes could fail
  `libmylite.embedded-same-process-concurrency` during InnoDB startup. In the
  serial CI-shaped non-ownerless embedded run, that test passed in
  `30.10 sec`.
- `bash -n tools/check-ci-production-builds`: passed.
- `tools/check-ci-production-builds`: passed and reported
  `ci_production_build_audit_ok`.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`: passed, 1/1 tests.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

## Acceptance Criteria

- CI no longer runs MariaDB embedded CTest processes in parallel.
- The production-build audit rejects future `ctest --parallel N` workflow
  changes.
- The open-close shared-memory layout test expects the current redo-state
  segment version.
- Production Release/MinSizeRel guards still run before embedded timing or
  test output.
