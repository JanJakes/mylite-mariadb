# MTR Coverage Inventory

## Problem

The MTR smoke harness has grown to a curated accepted list, but the roadmap
still needs a reproducible scale comparison against the imported MariaDB MTR
test inventory. Ad hoc `wc -l` commands answer the question once, but they do
not separate upstream tests from MyLite-owned profile/storage tests and they are
easy to misread as runtime pass evidence.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- Upstream `main` suite tests live under `mariadb/mysql-test/main/*.test`.
- Named MTR suite tests live under `mariadb/mysql-test/suite/<suite>/t/*.test`.
- MyLite-owned MTR tests live under `mariadb/mysql-test/suite/mylite/t` and
  should not be counted as imported upstream coverage.
- `tools/mylite-mtr-harness list` prints accepted baseline tests, including
  MyLite profile tests and imported upstream tests.
- `tools/mylite-mtr-harness list-storage` prints accepted MyLite-owned
  storage-routed tests.
- Recent candidate probes show that broad upstream DML, DDL, prepared-statement,
  function, and charset tests still often stop on intentionally disabled
  native MyISAM/InnoDB, host-file SQL I/O, server-only metadata, optional
  functions, sequence/view/runtime prerequisites, or embedded-skipped sections.

## Design

- Add `tools/mylite-mtr-harness coverage`.
- Do not build or run MTR. This command is an inventory summary, not pass/fail
  validation.
- Convert imported `*.test` paths to exact `suite.test` names using the same
  MTR naming shape as the accepted harness lists.
- Report:
  - total imported test files,
  - upstream imported test files excluding MyLite-owned tests,
  - MyLite-owned MTR test files,
  - accepted baseline, upstream baseline, MyLite baseline, storage, and total
    counts,
  - accepted upstream percentage,
  - unaccepted upstream test-file count.
- Keep strict pass evidence owned by `run` and discovery owned by `probe`.

## Compatibility Impact

No SQL behavior or compatibility status changes. This adds a reproducible
measurement for the MTR-scale comparison roadmap item and prevents MyLite-owned
tests from inflating upstream coverage counts.

## Single-File And Embedded-Lifecycle Impact

No `.mylite` file, sidecar, or embedded runtime lifecycle change. The command
only reads repository test paths and harness lists.

## Build, Size, And Dependencies

No production binary-size or dependency change. The implementation stays in the
existing Bash harness and uses standard shell tools already required by the
runner.

## Test And Verification Plan

- `bash -n tools/mylite-mtr-harness`.
- `tools/mylite-mtr-harness coverage`.
- `tools/mylite-mtr-harness list | wc -l`.
- `tools/mylite-mtr-harness list-storage | wc -l`.
- `find mariadb/mysql-test -name '*.reject' -print`.
- `git diff --check`.

## Acceptance Criteria

- `coverage` exits successfully without configuring or building MTR.
- The report distinguishes imported upstream tests from MyLite-owned MTR tests.
- The accepted baseline count matches `tools/mylite-mtr-harness list`.
- The accepted storage count matches `tools/mylite-mtr-harness list-storage`.
- The accepted upstream percentage is computed against upstream imported tests,
  not against MyLite-owned profile or storage tests.
- Existing `list`, `run`, `probe`, `list-storage`, `run-storage`, and
  `probe-storage` behavior is unchanged.

## Risks And Open Questions

- This is a file-inventory comparison, not a replacement for executing MTR.
- The current accepted upstream percentage is intentionally low because many
  upstream tests assume server or native-engine surfaces that the embedded
  MyLite profile excludes. Future comparison slices still need stable
  unsupported-surface normalization before broad MTR execution can become a
  useful default signal.
