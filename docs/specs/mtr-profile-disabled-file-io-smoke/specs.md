# MTR Profile Disabled File I/O Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with a MyLite-owned
`mylite.profile_disabled_file_io` case. The test covers host-file SQL I/O
surfaces that the MyLite MTR smoke profile intentionally compiles out of the
embedded server build.

## Non-Goals

- `libmylite` file-import/export preflight coverage.
- Enabling server-side host-file import or export.
- Testing `LOAD DATA`; that is covered by the broader disabled-surface MTR
  smoke.
- Normalizing upstream MTR tests that require host-file SQL I/O.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `cmake/mariadb-embedded-baseline.cmake` disables
  `MYLITE_WITH_SQL_FILE_IO`.
- `mariadb/sql/CMakeLists.txt` turns the C preprocessor symbol into
  `MYLITE_WITH_SQL_FILE_IO=0` for the embedded profile.
- `mariadb/sql/sql_class.cc` reports `ER_NOT_SUPPORTED_YET` for
  `SELECT ... INTO OUTFILE` and `SELECT ... INTO DUMPFILE` when SQL file I/O
  is disabled.
- `mariadb/sql/item_create.cc` omits `LOAD_FILE()` registration when
  `MYLITE_WITH_SQL_FILE_IO` is disabled, so calls resolve as missing functions
  in the raw embedded profile.
- Verified command:
  `tools/mylite-mtr-harness run mylite.profile_disabled_file_io`.

## Compatibility Impact

The roadmap and harness docs can say the opt-in MTR smoke runner covers
selected profile-disabled host-file SQL I/O behavior. This remains raw
embedded-profile evidence. `libmylite` user-facing diagnostics stay covered by
the public API tests.

## Design

- Add `mariadb/mysql-test/suite/mylite/t/profile_disabled_file_io.test` and
  its expected result file.
- Add `mylite.profile_disabled_file_io` to the default curated MTR smoke list
  near the other MyLite profile tests.
- Assert that:
  - `SELECT ... INTO OUTFILE` rejects with `ER_NOT_SUPPORTED_YET`;
  - `SELECT ... INTO DUMPFILE` rejects with `ER_NOT_SUPPORTED_YET`;
  - `LOAD_FILE()` is not registered in the raw profile.

## File Lifecycle

No MyLite `.mylite` file, companion-file, or storage lifecycle behavior
changes. The rejected OUTFILE and DUMPFILE paths must not create host files,
and the test runs inside the MariaDB MTR var directory managed by
`tools/mylite-mtr-harness`.

## Embedded Lifecycle And API

No public C API change. This slice covers the raw MariaDB embedded smoke
profile.

## Build, Size, And Dependencies

No production dependency or binary-size change. The new test exercises existing
compile-time profile switches and disabled file-I/O code paths.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run mylite.profile_disabled_file_io`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `test ! -e /tmp/mylite-mtr-disabled-file-io.out`
- `test ! -e /tmp/mylite-mtr-disabled-file-io.dump`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `mylite.profile_disabled_file_io`.
- The new MyLite MTR test passes under the MyLite MTR smoke profile.
- Existing curated MTR smoke tests still pass.
- The rejected file-export paths do not create `/tmp` output files.
- Docs keep the claim scoped to profile-disabled host-file SQL I/O behavior.

## Risks And Open Questions

- This is intentionally not a compatibility claim for server-side host-file
  SQL I/O. MyLite keeps those features out of the embedded profile.
- The raw profile reports `LOAD_FILE()` as a missing function; `libmylite`
  still owns clearer user-facing file-I/O diagnostics before MariaDB
  execution.
