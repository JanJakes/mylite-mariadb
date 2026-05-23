# MTR Main Bootstrap Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with `main.1st`. This adds
MariaDB's canonical baseline check that the bootstrap schema contains the
expected databases and `mysql` schema tables under the embedded smoke profile.

## Non-Goals

- Re-enabling native InnoDB, CSV, MyISAM, MRG_MyISAM, or partition support.
- Treating server-owned system tables as MyLite application storage.
- Running the test through MyLite routed storage.
- Changing MyLite SQL behavior, storage behavior, public APIs, or file format.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/main/1st.test` runs `SHOW DATABASES` and
  `SHOW TABLES IN mysql` as an early upstream bootstrap-schema sanity check.
- The MyLite embedded MTR profile intentionally omits native CSV and InnoDB
  storage-engine pieces. The first probe differed only by missing
  `general_log`, `innodb_index_stats`, `innodb_table_stats`, `slow_log`, and
  `transaction_registry` rows.
- The same trimmed bootstrap shape is already covered by the MyLite-owned
  `mylite.bootstrap_schema` test. Adding `main.1st` keeps the upstream smoke
  list aligned with the profile-specific expected schema.

## Design

- Update `main.1st` expected output to match the MyLite embedded profile's
  intentionally trimmed bootstrap schema.
- Add `main.1st` to the curated MTR smoke list immediately after the MyLite
  profile disabled-surface tests.

## Compatibility Impact

The opt-in embedded MTR smoke runner gains MariaDB upstream baseline coverage
for bootstrap databases and `mysql` schema tables under MyLite's trimmed
embedded profile. This does not imply support for the omitted native engine or
server-log tables.

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

- `tools/mylite-mtr-harness probe main.1st`
- `tools/mylite-mtr-harness run main.1st`
- `tools/mylite-mtr-harness run`
- `tools/mylite-mtr-harness list | wc -l`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- `main.1st` reports an MTR pass under strict harness execution.
- The default MTR smoke list includes `main.1st`.
- Expected-output changes are limited to the MyLite embedded profile's
  intentionally omitted system tables.
- No `.reject` files remain.

## Verification Results

- `tools/mylite-mtr-harness probe main.1st`: passed.
- `tools/mylite-mtr-harness run main.1st`: passed.
- `tools/mylite-mtr-harness run`: passed all 8 MyLite profile tests plus 192
  selected `main` MTR smoke tests.
- `tools/mylite-mtr-harness list | wc -l`: `200`.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no output.
- `git diff --check`: passed.

## Risks And Follow-Up

This overlaps with `mylite.bootstrap_schema`, but keeps an upstream MariaDB
baseline test in the curated `main` suite. Broader bootstrap behavior still
belongs in MyLite-owned embedded lifecycle and server-surface tests.
