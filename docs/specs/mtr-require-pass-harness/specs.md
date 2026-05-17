# MTR Require-Pass Harness

## Goal

Harden `tools/mylite-mtr-harness` so a selected `suite.test` must report an
actual MTR `[ pass ]` result. MTR exits successfully when an exact selected
test is skipped, which is useful for ad hoc investigation but unsafe for a
curated compatibility list.

## Non-Goals

- Adding skipped tests to the curated list.
- Parsing or normalizing full MTR result logs.
- Turning MTR into a default compatibility harness group.
- Changing MariaDB test sources.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb-test-run.pl --do-test=^func_str$` exits zero under the MyLite MTR
  smoke profile even though `main.func_str` is skipped because the Sequence
  engine is unavailable.
- The MyLite curated list should represent tests that actually ran and passed,
  not tests that were selected but skipped by feature checks.
- MTR writes a stable per-test summary line such as
  `main.func_concat [ pass ]`, which the harness can require after each exact
  selected test run.

## Compatibility Impact

No compatibility status changes. This prevents accidental overstatement of MTR
coverage in future curated-list edits.

## Design

Capture each `mariadb-test-run.pl` invocation through `tee` into a temporary
file. If MTR exits nonzero, remove the temporary file and fail. If MTR exits
zero but the expected exact `suite.test` line does not contain `[ pass ]`, also
remove the temporary file and fail with a clear harness diagnostic.

## File Lifecycle

The harness creates one temporary output file per selected test under
`${TMPDIR:-/tmp}` and removes it on pass, MTR failure, and pass-check failure.
No MyLite `.mylite` file or production runtime lifecycle changes.

## Embedded Lifecycle And API

No `libmylite` API change.

## Build, Size, And Dependencies

No dependency or production binary-size change. The implementation uses POSIX
tools already required for the Bash harness.

## Test Plan

- `tools/mylite-mtr-harness run main.func_str` should fail because the test is
  skipped rather than passed.
- `tools/mylite-mtr-harness run` should pass the curated list.
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- Skipped selected tests fail with a harness diagnostic.
- The default curated MTR list still passes.
- Temporary output capture files do not remain after harness runs.

## Risks And Open Questions

- The pass-line check is intentionally narrow. If MTR changes its summary
  format, the harness will fail closed and need an update.
