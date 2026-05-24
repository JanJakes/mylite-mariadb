# MTR Sysvar Static Global Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with accepted upstream
system-variable tests for retained static global metadata:

- `sys_vars.license_basic`
- `sys_vars.system_time_zone_basic`

## Non-Goals

- Broad system-variable MTR coverage.
- Changing MyLite's default system-variable values or runtime policy.
- Re-enabling disabled server surfaces such as binlog, replication, native
  engines, account management, or daemon-owned logs.
- Running these tests against MyLite storage-engine routing.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/suite/sys_vars/t/license_basic.test` checks the retained
  global `license` variable, its read-only behavior, and
  `INFORMATION_SCHEMA.GLOBAL_VARIABLES` metadata.
- `mariadb/mysql-test/suite/sys_vars/t/system_time_zone_basic.test` checks the
  retained global `system_time_zone` variable, its read-only behavior, and
  `INFORMATION_SCHEMA.GLOBAL_VARIABLES` metadata.
- Both selected tests passed under the MyLite MTR smoke profile without
  upstream source changes.
- Probed candidates intentionally left out of accepted coverage:
  - `main.fetch_first` and `main.delete_returning` were skipped by upstream MTR
    because their test files source `include/have_sequence.inc`; those whole
    upstream files are tracked as disabled Sequence-engine non-coverage.
  - `main.intersect_all` failed on explicit native `ENGINE=MyISAM` statements.
  - `main.func_json` failed during embedded bootstrap because the test requires
    native InnoDB options.

## Compatibility Impact

The compatibility matrix can say the opt-in embedded MTR smoke runner covers
selected static global system-variable metadata, including read-only access and
`INFORMATION_SCHEMA.GLOBAL_VARIABLES` consistency. This remains MariaDB
embedded baseline coverage and does not change SQL, C API, storage-engine, or
file-format behavior.

## Design

- Add the selected passing tests to `tools/mylite-mtr-harness`'s default
  curated list.
- Do not modify upstream MariaDB test files.
- Keep skipped, native-engine, and bootstrap-incompatible candidates outside
  accepted coverage.

## File Lifecycle

No MyLite `.mylite` file format or runtime lifecycle changes. The tests run
inside `build/mariadb-mtr-smoke/mysql-test/var`.

## Embedded Lifecycle And API

No `libmylite` API change. The slice expands opt-in MariaDB embedded MTR
baseline coverage only.

## Build, Size, And Dependencies

No dependency or production binary-size change. The opt-in MTR build tree can
still be reclaimed with `rm -rf build/mariadb-mtr-smoke` or `rm -rf build`.

## Test Plan

- `tools/mylite-mtr-harness probe main.fetch_first main.intersect_all main.delete_returning main.func_json sys_vars.license_basic sys_vars.system_time_zone_basic`
- `tools/mylite-mtr-harness run sys_vars.license_basic sys_vars.system_time_zone_basic`
- `tools/mylite-mtr-harness coverage`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes `sys_vars.license_basic` and
  `sys_vars.system_time_zone_basic`.
- Both selected tests pass under the MyLite MTR smoke profile.
- Coverage inventory counts increase accepted upstream baseline coverage by two
  files without changing known unsupported counts.
- No upstream MariaDB test files are modified for this slice.
- Docs keep the claim scoped to curated opt-in MTR smoke coverage.

## Verification Results

- `tools/mylite-mtr-harness probe main.fetch_first main.intersect_all main.delete_returning main.func_json sys_vars.license_basic sys_vars.system_time_zone_basic`: 2 passed, 2 failed, and 2 skipped.
- `tools/mylite-mtr-harness run sys_vars.license_basic sys_vars.system_time_zone_basic`: passed.
- `tools/mylite-mtr-harness coverage`: 5,901 upstream test files, 415 accepted
  upstream baseline tests, 8 accepted MyLite profile tests, 19 accepted MyLite
  storage-routed tests, 442 accepted total tests, 4,613 known unsupported
  upstream tests, and 873 unclassified upstream tests.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no reject files.
- `git diff --check`

## Risks And Open Questions

- Broader static/global system-variable coverage still needs separate probe
  batches because many upstream `sys_vars` rows exercise disabled server or
  native-engine surfaces.
- This remains MariaDB embedded baseline coverage and does not prove MyLite
  storage-routing behavior.
