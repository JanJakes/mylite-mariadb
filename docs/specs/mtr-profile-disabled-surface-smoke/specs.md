# MTR Profile Disabled Surface Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with a MyLite-owned
`mylite.profile_disabled_surfaces` case. The test covers SQL surfaces that the
MyLite MTR smoke profile intentionally compiles out of the embedded server
build.

## Non-Goals

- Full unsupported-surface coverage.
- `libmylite` SQL policy preflight coverage.
- Running MTR against MyLite storage-engine routing.
- Enabling disabled server-oriented runtimes to satisfy upstream MTR cases.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `cmake/mariadb-embedded-baseline.cmake` disables dynamic plugin loading,
  `LOAD DATA`, SQL HANDLER, SQL sequence runtime, JSON table-function
  execution, GIS SQL functions, and other server-oriented surfaces.
- `cmake/mariadb-mtr-smoke.cmake` keeps the same embedded baseline but
  re-enables view, stored-program, trigger, and binlog-system-variable runtime
  pieces required by mysql-test bootstrap SQL.
- `mariadb/sql/mylite_sql_load_disabled.cc` returns
  `ER_NOT_SUPPORTED_YET` for `LOAD DATA` and `LOAD XML` execution with a
  MyLite embedded-profile diagnostic.
- `mariadb/sql/mylite_sql_handler_disabled.cc` returns
  `ER_NOT_SUPPORTED_YET` for SQL HANDLER open/read/close paths.
- `mariadb/sql/mylite_sql_sequence_disabled.cc` returns
  `ER_NOT_SUPPORTED_YET` for sequence creation, sequence table validation, and
  sequence value access.
- `mariadb/sql/mylite_json_table_disabled.cc` returns
  `ER_NOT_SUPPORTED_YET` for JSON_TABLE execution.
- `mariadb/sql/mylite_gis_sql_functions_disabled.cc` resolves trimmed GIS
  helper functions as missing stored functions rather than linking GIS runtime.
- Verified command:
  `tools/mylite-mtr-harness run mylite.profile_disabled_surfaces`.

## Compatibility Impact

The roadmap and harness docs can say the opt-in MTR smoke runner covers
selected profile-disabled SQL surfaces in the MyLite suite. This is embedded
profile evidence only. It does not replace `libmylite` unsupported-surface
tests, and it does not prove raw MySQL/MariaDB compatibility for disabled
server-oriented features.

## Design

- Add `mariadb/mysql-test/suite/mylite/t/profile_disabled_surfaces.test` and
  its expected result file.
- Add `mylite.profile_disabled_surfaces` to the default curated MTR smoke list,
  immediately after the bootstrap-schema check.
- Keep the case focused on stable disabled-profile behavior:
  - dynamic loading reports `NO`;
  - `LOAD DATA` rejects through the compiled-out load executor;
  - SQL HANDLER rejects through the compiled-out handler runtime;
  - SQL sequences reject through the compiled-out sequence runtime;
  - JSON_TABLE rejects through the compiled-out table-function runtime;
  - representative GIS function lookup fails because GIS SQL functions are
    omitted.

## File Lifecycle

No MyLite `.mylite` file, companion-file, or storage lifecycle behavior
changes. The test runs inside the MariaDB MTR var directory managed by
`tools/mylite-mtr-harness`.

## Embedded Lifecycle And API

No public C API change. This slice covers the raw MariaDB embedded smoke
profile, not `libmylite` handles or diagnostics.

## Build, Size, And Dependencies

No production dependency or binary-size change. The MTR smoke profile continues
to build the same support targets, and the new test only exercises already
compiled disabled-runtime stubs.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run mylite.profile_disabled_surfaces`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `mylite.profile_disabled_surfaces`.
- The new MyLite MTR test passes under the MyLite MTR smoke profile.
- Existing curated MTR smoke tests still pass.
- Docs keep the claim scoped to profile-disabled MTR smoke behavior.

## Risks And Open Questions

- This test intentionally covers a small representative subset. The broader
  unsupported-surface policy remains covered by `libmylite` tests and should
  only move into MTR when a raw embedded-profile assertion is useful.
- Some upstream MTR cases that look similar are skipped because they require
  the disabled Sequence engine or non-embedded client behavior. Those skipped
  cases should not be counted as coverage.
