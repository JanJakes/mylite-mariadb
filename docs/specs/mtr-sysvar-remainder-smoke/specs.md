# MTR Sysvar Remainder Smoke

## Goal

Promote the remaining unclassified upstream `sys_vars` MTR rows that pass under
the MyLite embedded smoke profile:

- `sys_vars.mdev_15935`
- `sys_vars.read_only_basic`
- `sys_vars.show_vs_valstr`
- `sys_vars.sql_mode_func`
- `sys_vars.timestamp_basic`
- `sys_vars.timestamp_func`
- `sys_vars.timestamp_sysdate_is_now_func`
- `sys_vars.tx_compatibility`

## Non-Goals

- Broad server-system-variable compatibility beyond the probed rows.
- Changing MyLite runtime policy for read-only mode, SQL modes, timestamp
  defaults, or transaction compatibility.
- Re-enabling disabled server surfaces or native engines.
- Running these rows against MyLite storage-engine routing.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/mysql-test/suite/sys_vars/t/read_only_basic.test` checks retained
  `read_only` variable visibility and assignment behavior under the embedded
  profile without requiring account-policy enforcement.
- `mariadb/mysql-test/suite/sys_vars/t/sql_mode_func.test` checks expression
  access to `sql_mode`.
- `mariadb/mysql-test/suite/sys_vars/t/timestamp_basic.test`,
  `timestamp_func.test`, and `timestamp_sysdate_is_now_func.test` check
  timestamp variable defaults and function-style access.
- `mariadb/mysql-test/suite/sys_vars/t/tx_compatibility.test` checks the
  retained transaction-compatibility system variable surface.
- `mariadb/mysql-test/suite/sys_vars/t/show_vs_valstr.test` and
  `mdev_15935.test` cover retained SHOW/value-string metadata behavior.
- All eight selected tests passed under the MyLite MTR smoke profile without
  upstream source changes.

## Compatibility Impact

The opt-in embedded MTR smoke runner now covers the remaining probed
system-variable rows that are compatible with the current trimmed profile. This
is MariaDB embedded baseline coverage only; it does not change SQL, C API,
storage-engine, file-format, or server-surface policy.

## Design

- Add the selected passing tests to `tools/mylite-mtr-harness`'s default
  curated list.
- Do not modify upstream MariaDB test files.
- Keep disabled or profile-specific `sys_vars` rows in the known unsupported
  inventory.

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

- `tools/mylite-mtr-harness probe sys_vars.mdev_15935 sys_vars.read_only_basic sys_vars.show_vs_valstr sys_vars.sql_mode_func sys_vars.timestamp_basic sys_vars.timestamp_func sys_vars.timestamp_sysdate_is_now_func sys_vars.tx_compatibility`
- `tools/mylite-mtr-harness run sys_vars.mdev_15935 sys_vars.read_only_basic sys_vars.show_vs_valstr sys_vars.sql_mode_func sys_vars.timestamp_basic sys_vars.timestamp_func sys_vars.timestamp_sysdate_is_now_func sys_vars.tx_compatibility`
- `tools/mylite-mtr-harness coverage`
- `tools/mylite-mtr-harness list-unclassified`
- `bash -n tools/mylite-mtr-harness`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes all eight selected `sys_vars` tests.
- All selected tests pass under the MyLite MTR smoke profile.
- No `sys_vars.*` rows remain unclassified in the MTR inventory.
- Coverage inventory counts increase accepted upstream baseline coverage by
  eight files without changing known unsupported counts.
- No upstream MariaDB test files are modified for this slice.

## Verification Results

- `tools/mylite-mtr-harness probe sys_vars.mdev_15935 sys_vars.read_only_basic sys_vars.show_vs_valstr sys_vars.sql_mode_func sys_vars.timestamp_basic sys_vars.timestamp_func sys_vars.timestamp_sysdate_is_now_func sys_vars.tx_compatibility`: 8 passed, 0 failed, and 0 skipped.
- `tools/mylite-mtr-harness run sys_vars.mdev_15935 sys_vars.read_only_basic sys_vars.show_vs_valstr sys_vars.sql_mode_func sys_vars.timestamp_basic sys_vars.timestamp_func sys_vars.timestamp_sysdate_is_now_func sys_vars.tx_compatibility`: passed.
- `tools/mylite-mtr-harness coverage`: 5,901 upstream test files, 423 accepted
  upstream baseline tests, 8 accepted MyLite profile tests, 19 accepted MyLite
  storage-routed tests, 450 accepted total tests, 4,613 known unsupported
  upstream tests, and 865 unclassified upstream tests.
- `tools/mylite-mtr-harness list-unclassified`: no `sys_vars.*` rows remain.
- `bash -n tools/mylite-mtr-harness`: passed.
- `find mariadb/mysql-test -name '*.reject' -print`: no reject files.
- `git diff --check`

## Risks And Open Questions

- This closes the current imported `sys_vars` inventory but does not imply full
  application-level system-variable compatibility.
- Broader system-variable behavior should continue to be proven through
  targeted first-party compatibility tests where MyLite policy diverges from a
  daemon server.
