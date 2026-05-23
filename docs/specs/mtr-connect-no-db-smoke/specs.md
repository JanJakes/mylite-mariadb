# MTR Connect-No-DB Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.connect-no-db`. This
adds curated embedded baseline coverage for opening a connection without a
selected database and verifying that `DATABASE()` reports `NULL`.

## Non-Goals

- Broad connection-management or authentication MTR promotion.
- Running the test through MyLite routed storage.
- Changing MyLite SQL behavior, storage behavior, public APIs, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/connect-no-db.test` covers MDEV-34226 by connecting
  without a selected database and selecting `DATABASE()`.
- The test passes under the MyLite embedded MTR profile without source or
  expected-result normalization.
- This is baseline MariaDB session-state evidence. It does not replace
  first-party `libmylite` open/close, schema selection, or catalog-backed
  schema tests.

## Design

- Add `main.connect-no-db` to the curated MTR smoke list immediately after the
  MyLite profile disabled-surface tests and before expression-oriented main
  tests.
- Keep the upstream test and result unchanged.

## Compatibility Impact

The opt-in embedded MTR smoke runner gains selected MariaDB baseline coverage
for no-default-database connection state and the `DATABASE()` result in that
state. This is not routed-storage evidence and does not alter MyLite schema
catalog behavior.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file lifecycle change. The test runs in the MTR smoke vardir and
uses the baseline embedded MTR profile.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.connect-no-db`
- `tools/mylite-mtr-harness run main.connect-no-db`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.connect-no-db` reports an MTR pass under strict harness execution.
- The default MTR smoke list includes `main.connect-no-db`.
- No upstream test-source or expected-result changes are needed.
- No `.reject` files remain.

## Verification Results

- `tools/mylite-mtr-harness probe main.connect-no-db`: passed.
- `tools/mylite-mtr-harness run main.connect-no-db`: passed.
- `tools/mylite-mtr-harness run`: passed all 8 MyLite profile tests plus 191
  selected `main` MTR smoke tests.
- `tools/mylite-mtr-harness list | wc -l`: `199`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The test remains baseline-only. MyLite-owned schema-selection behavior remains
covered by first-party public API, prepared-statement, and routed-storage
schema tests.
