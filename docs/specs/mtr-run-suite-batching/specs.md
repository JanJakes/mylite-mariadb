# MTR Run Suite Batching

## Problem

`tools/mylite-mtr-harness run` has grown to many accepted tests. Running one
MTR process per selected test keeps failure attribution simple, but it repeats
MTR feature checks, var-dir setup, bootstrap installation, and process startup
for every accepted case. MariaDB MTR is designed to run multiple tests in one
suite invocation, so strict accepted-coverage runs can be faster without
weakening per-test pass assertions.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/mariadb-test-run.pl` accepts `--suite=<suite>` and
  `--do-test=<regex>`.
- The existing MyLite harness already anchors exact case names and asserts the
  MTR `[ pass ]` summary line for every selected test.
- The accepted default list currently spans the `mylite` and `main` suites.
- `probe` needs one MTR invocation per candidate so failures, skips, and
  generated rejects remain isolated during discovery.

## Design

- Keep `probe` one selected candidate per MTR process.
- Change strict `run` to group selected tests by suite, preserving the first
  suite order seen in the selected list.
- Build a `--do-test=^(case_a|case_b|...)$` regex per suite.
- After each suite run, assert that every selected `suite.case` reports an MTR
  `[ pass ]` line. A missing pass line still fails the harness.
- Keep failure artifacts from strict `run` available for debugging.

## Compatibility Impact

No compatibility status change and no new SQL coverage. This only improves the
accepted MTR smoke runner's execution cost.

## Single-File And Embedded-Lifecycle Impact

No `.mylite` file, sidecar, or embedded runtime lifecycle change. The same MTR
work directory under `build/mariadb-mtr-smoke/mysql-test/var` is used.

## Build, Size, And Dependencies

No dependency or production binary-size change. The implementation stays in the
existing Bash harness.

## Test And Verification Plan

- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`.
- `tools/mylite-mtr-harness run mylite.bootstrap_schema main.cast main.prepare`.
- `tools/mylite-mtr-harness probe main.prepare`.
- `tools/mylite-mtr-harness run`.
- `find mariadb/mysql-test -name '*.reject' -print`.
- `git diff --check`.

## Acceptance Criteria

- Strict `run` invokes MTR once per selected suite instead of once per selected
  test.
- Strict `run` still fails if any selected test lacks an MTR `[ pass ]` line.
- `probe` keeps one-candidate-at-a-time execution.
- The full curated MTR smoke list still passes.

## Risks And Open Questions

- Suite batching can expose test-order coupling that one-process-per-test did
  not. The full curated list must pass before accepting this slice.
- If a future accepted test needs a dedicated MTR process, it may need a
  per-test isolation marker rather than disabling suite batching globally.
