# MTR Profile Disabled Dynamic Columns Smoke

## Goal

Extend the opt-in MariaDB MTR smoke runner with a MyLite-owned
`mylite.profile_disabled_dynamic_columns` case. The test covers raw embedded
profile behavior for MariaDB dynamic-column SQL functions when the default
MyLite embedded profile compiles out the packed dynamic-column runtime.

## Non-Goals

- Re-enabling MariaDB dynamic columns.
- Changing public `libmylite` unsupported-surface diagnostics.
- Full dynamic-column compatibility coverage.
- Changing parser or SQL function registration for dynamic-column syntax.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- [Dynamic Column Trim](../dynamic-column-trim/specs.md) keeps parser tokens,
  grammar, headers, and SQL item classes intact, while replacing
  `mariadb/mysys/ma_dyncol.c` with
  `mariadb/mysys/mylite_ma_dyncol_disabled.c` in the default embedded profile.
- `mariadb/mysys/ma_dyncol.c` initializes output `DYNAMIC_COLUMN` strings in
  `mariadb_dyncol_create_many_*()` when `new_string` is true so callers can
  call `mariadb_dyncol_free()` even after errors.
- `mariadb/sql/item_strfunc.cc` calls `mariadb_dyncol_create_many_*()` for
  `COLUMN_CREATE()` and frees the output string on error.
- `mariadb/sql/item_cmpfunc.cc` treats `ER_DYNCOL_FORMAT` from
  `COLUMN_CHECK()` as a false validity result, not as a raised SQL error.
- The initial probe exposed a crash in the disabled shim because it returned
  `ER_DYNCOL_FORMAT` without initializing the output dynamic string expected by
  the SQL caller's cleanup path.

## Compatibility Impact

This is raw MariaDB embedded-profile evidence. Public MyLite direct and
prepared execution continue to reject dynamic-column functions before MariaDB
execution, as documented in [Compatibility](../../COMPATIBILITY.md). The MTR
test proves the disabled embedded fallback remains fail-closed and
cleanup-safe if the raw MariaDB SQL item path reaches the dynamic-column C API.

## Design

- Make the disabled dynamic-column shim preserve MariaDB's output initialization
  contract for create functions with `new_string=true`.
- Initialize JSON output strings before returning the disabled error so callers
  can safely free them if needed.
- Keep scalar conversion outputs deterministic on disabled errors.
- Add `mylite.profile_disabled_dynamic_columns` to the default curated MTR
  smoke list near the other MyLite profile-disabled tests.
- Assert representative SQL behavior:
  - `COLUMN_CREATE()`, `COLUMN_EXISTS()`, `COLUMN_GET()`, `COLUMN_JSON()`, and
    `COLUMN_LIST()` raise MariaDB's wrong-format dynamic-column error;
  - `COLUMN_CHECK('')` returns `0`, matching MariaDB's validity-check handling
    for wrong-format packed values.

## File Lifecycle

No MyLite `.mylite` file, companion-file, or storage lifecycle behavior
changes. The test runs inside the MariaDB MTR var directory managed by
`tools/mylite-mtr-harness`.

## Embedded Lifecycle And API

No public C API change. The inherited MariaDB dynamic-column C API symbols
remain linkable in the disabled embedded archive, but return disabled
dynamic-column result codes while preserving cleanup-safe output state.

## Build, Size, And Dependencies

No new dependency or intentional binary-size change. The production change is a
small robustness fix in an existing disabled shim.

## Test Plan

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run mylite.profile_disabled_dynamic_columns`
- `tools/mylite-mtr-harness run`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Verification Results

Completed on 2026-05-22:

- `tools/mylite-mtr-harness list`
- `tools/mylite-mtr-harness run mylite.profile_disabled_dynamic_columns`
- `tools/mylite-mtr-harness run`
- `tools/mariadb-embedded-build all`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build libmariadbd.a`
- `cmake --build --preset embedded-dev`
- `ctest --preset embedded-dev --output-on-failure`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `ctest --preset embedded-dev -R '^libmylite\.embedded-statement$' --output-on-failure`
- `ctest --preset storage-smoke-dev -R '^libmylite\.embedded-statement$' --output-on-failure`
- `cmake --preset dev && cmake --build --preset dev && ctest --preset dev --output-on-failure`
- `bash -n tools/mariadb-embedded-build tools/mylite-mtr-harness tools/mylite-compat-harness tools/mylite-size-report`
- `git clang-format --diff HEAD -- mariadb/mysys/mylite_ma_dyncol_disabled.c`
- `find mariadb/mysql-test -name '*.reject' -print`
- `git diff --check`

## Acceptance Criteria

- The default MTR smoke list includes
  `mylite.profile_disabled_dynamic_columns`.
- The disabled shim no longer crashes when raw embedded SQL reaches
  `COLUMN_CREATE()` and MariaDB frees the failed output.
- The new MyLite MTR test passes under the MyLite MTR smoke profile.
- Existing curated MTR smoke tests still pass.
- Docs keep the claim scoped to profile-disabled raw embedded behavior.

## Risks And Open Questions

- This smoke does not make MariaDB dynamic columns a supported MyLite feature.
  The project can revisit that decision only if a concrete application suite
  needs packed dynamic-column BLOB compatibility.
