# MTR Statistics-Close Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.statistics_close`. This
adds curated embedded baseline coverage for a historical handler close
lifecycle regression where concurrent `RENAME TABLE` and `FLUSH TABLES`
operations could crash while closing table statistics state.

## Non-Goals

- Broad statistics-table or optimizer-statistics MTR promotion.
- Running the test through MyLite routed storage.
- Enabling MariaDB persistent statistics tables in MyLite sessions.
- Changing MyLite SQL behavior, storage behavior, public APIs, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/statistics_close.test` covers MDEV-16123 and
  MDEV-13828 by creating a table, renaming it from a second connection while
  the default connection runs `FLUSH TABLES`, and then cleaning up the renamed
  table.
- The test passes under the MyLite embedded MTR profile without source or
  expected-result normalization.
- This is baseline MariaDB lifecycle evidence. It does not assert MyLite-owned
  persistent statistics, and it does not change MyLite's current
  `use_stat_tables=NEVER` file-backed session default.

## Design

- Add `main.statistics_close` to the curated MTR smoke list near other DDL and
  metadata lifecycle tests.
- Keep the upstream test and result unchanged.

## Compatibility Impact

The opt-in embedded MTR smoke runner gains selected MariaDB baseline coverage
for table close lifecycle behavior around concurrent `RENAME TABLE` and
`FLUSH TABLES`. This is not routed-storage evidence and does not imply support
for persistent MariaDB statistics tables in MyLite-owned files.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file lifecycle change. The test runs in the MTR smoke vardir and
uses the baseline embedded MTR profile.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. Routed storage `RENAME TABLE` and table-close behavior
remain covered by first-party MyLite storage tests and storage-routed MTR
cases.

## Build, Size, And Dependencies

No production dependency, binary-size, or default-build change.

## Test Plan

- `tools/mylite-mtr-harness probe main.statistics_close`
- `tools/mylite-mtr-harness run main.statistics_close`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.statistics_close` reports an MTR pass under strict harness execution.
- The default MTR smoke list includes `main.statistics_close`.
- No upstream test-source or expected-result changes are needed.
- No `.reject` files remain.

## Verification Results

- `tools/mylite-mtr-harness probe main.statistics_close`: passed.
- `tools/mylite-mtr-harness run main.statistics_close`: passed.
- `tools/mylite-mtr-harness run`: passed all 8 MyLite profile tests plus 190
  selected `main` MTR smoke tests.
- `tools/mylite-mtr-harness list | wc -l`: `198`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

The test remains baseline-only. Future routed storage coverage should continue
to assert MyLite catalog metadata, sidecar absence, and table-close behavior
through MyLite-owned storage tests where the handler and `.mylite` file
lifecycle are directly exercised.
