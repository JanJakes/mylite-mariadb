# UDF Runtime Trim

## Problem

MyLite rejects non-table object DDL and dynamic plugin installation through the
public SQL policy, but the embedded archive still links MariaDB's UDF runtime.
That runtime scans `mysql.func`, opens dynamic libraries, tracks process-global
UDF state, and constructs UDF execution items.

UDFs are plugin-like server extension points, not part of the current
file-owned embedded runtime. Keeping their lookup, DDL, and execution bodies in
the default archive adds dead code behind an unsupported surface.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/libmylite/src/database.cc` rejects `CREATE FUNCTION`, `DROP
  FUNCTION`, and dynamic plugin installation before MariaDB execution.
- `mariadb/sql/sql_yacc.yy` probes `find_udf()` while parsing generic function
  calls and can create `Create_udf_func` items when a registered UDF matches.
- `mariadb/sql/item_create.cc` defines `Create_udf_func`, which constructs
  `Item_func_udf*` and `Item_sum_udf*` instances.
- `mariadb/sql/item_func.cc` and `mariadb/sql/item_sum.cc` implement UDF scalar
  and aggregate execution via `udf_handler`.
- `mariadb/sql/sql_udf.cc` owns UDF startup/shutdown, the global UDF hash,
  `mysql.func` loading, dynamic library symbol lookup, and
  `mysql_create_function()` / `mysql_drop_function()`.
- Before this slice, `mariadb/libmysqld/CMakeLists.txt` included
  `../sql/sql_udf.cc` in `SQL_EMBEDDED_SOURCES`.

## Design

- Add `MYLITE_WITH_UDF_RUNTIME`, defaulting to `ON` for upstream-compatible
  embedded builds.
- Set `MYLITE_WITH_UDF_RUNTIME=OFF` in
  `cmake/mariadb-embedded-baseline.cmake`.
- When disabled in `mariadb/libmysqld/CMakeLists.txt`, define
  `MYLITE_WITH_UDF_RUNTIME=0` and remove `../sql/sql_udf.cc` from
  `SQL_EMBEDDED_SOURCES`.
- Guard UDF parser lookup, UDF item construction, and UDF scalar/aggregate
  execution bodies behind `MYLITE_WITH_UDF_RUNTIME`.
- Keep `sql_udf.h` inline no-op or fail-closed stubs for retained startup,
  shutdown, DDL, and unknown-function resolution references.
- Preserve current unknown function behavior: with no UDF registry, names that
  are not native functions continue falling through to stored-function
  resolution and diagnostics.

## Affected Subsystems

- MariaDB embedded build profile and source list.
- Generic function-call parser actions.
- UDF scalar and aggregate item construction/execution.
- UDF DDL helpers and startup/shutdown hooks.
- Public server-surface and non-table object SQL policy tests.
- Compatibility, size-profile, and roadmap documentation.

## MySQL/MariaDB Compatibility Impact

Dynamic UDF loading, registration, lookup, and execution are out of scope for
the current MyLite embedded profile. Public direct and prepared calls reject UDF
DDL before MariaDB execution. Unknown SQL function calls remain ordinary MariaDB
unknown/stored-function resolution failures; MyLite does not add a new public
UDF-specific API.

The normal MariaDB `sql` target keeps the full UDF runtime for comparison
builds.

## Single-File And Embedded-Lifecycle Impact

This removes a `mysql.func` and dynamic library lifecycle from the embedded
profile. No `.mylite` catalog or file-format change is introduced.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change. Existing public rejection for
non-table object SQL remains `MYLITE_ERROR`, SQLSTATE `HY000`, MariaDB errno
`0`, and a stable MyLite diagnostic.

## Storage-Engine Routing Impact

None. UDF metadata is not routed through MyLite storage and has no catalog-backed
representation.

## Binary-Size Impact

Before this slice, the default archive contained `sql_udf.cc.o` and linked UDF
item construction/execution bodies from retained SQL objects.

Measured on 2026-05-15:

| Profile | Archive size | Members | Delta from stored-program baseline |
| --- | ---: | ---: | ---: |
| Default embedded | 27,830,224 bytes / 26.54 MiB | 682 | -91,848 bytes / -1 member |
| Storage-smoke | 28,010,800 bytes / 26.71 MiB | 685 | -91,848 bytes / -1 member |

## License And Dependency Impact

No new dependency. The change only gates MariaDB-derived runtime code behind a
MyLite embedded profile option.

## Test And Verification Plan

- Add direct and prepared SQL-policy coverage for UDF `CREATE FUNCTION ...
  SONAME` DDL.
- Build and measure the default embedded profile.
- Build and measure the opt-in storage-smoke profile.
- Confirm both embedded archives omit `sql_udf.cc.o`.
- Confirm representative linked runtime symbols for `find_udf`,
  `Create_udf_func`, `Item_func_udf*`, and `Item_sum_udf*` are absent from
  embedded and storage-smoke linked test binaries.
- Run embedded and storage-smoke CTest presets.
- Run the `server-surface` compatibility report.
- Run the first-party size report.
- Build the normal MariaDB `sql` target to confirm the non-embedded server path
  still compiles with full UDF runtime.
- Run dev tests, format, shell syntax, diff, and tidy checks.

## Acceptance Criteria

- The default embedded cache records `MYLITE_WITH_UDF_RUNTIME=OFF`.
- Public direct and prepared UDF DDL rejection happens before MariaDB execution.
- The embedded archive omits `sql_udf.cc.o`.
- Ordinary MyLite open/close, direct SQL, prepared SQL, warnings, comparison,
  and storage-smoke coverage still pass.
- Documentation and compatibility matrix mark UDF runtime explicitly unsupported
  for the default embedded profile.
- Size measurements are recorded.

## Implementation Results

- `cmake/mariadb-embedded-baseline.cmake` disables
  `MYLITE_WITH_UDF_RUNTIME`.
- `mariadb/libmysqld/CMakeLists.txt` defines the embedded-profile macro and
  removes `../sql/sql_udf.cc` from `SQL_EMBEDDED_SOURCES` when disabled.
- Parser lookup, UDF item construction, and scalar/aggregate UDF execution
  bodies are guarded so the default embedded and storage-smoke profiles do not
  link UDF runtime symbols.
- `sql_udf.h` keeps inline startup/shutdown and DDL stubs for retained
  references.
- Direct and prepared UDF DDL rejection coverage was added beside the existing
  non-table object policy tests.

Verified with:

- `tools/mariadb-embedded-build configure`
- `tools/mariadb-embedded-build build`
- `tools/mariadb-embedded-build measure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build configure -DPLUGIN_MYLITE_SE=STATIC`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build measure`
- archive and linked-binary symbol checks for UDF runtime symbols in both
  embedded profiles
- `cmake --build --preset embedded-dev`
- `ctest --preset embedded-dev --output-on-failure`
- `cmake --build --preset storage-smoke-dev`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-compat-harness report server-surface`
- `tools/mylite-size-report`
- `cmake --build build/mariadb-embedded --target sql`
- `ctest --preset dev --output-on-failure`
- `cmake --build --preset dev --target format-check`
- `bash -n tools/mylite-compat-harness tools/mylite-mtr-harness tools/mariadb-embedded-build tools/mylite-size-report`
- `git diff --check`
- `cmake --build --preset dev --target tidy`

## Risks And Unresolved Questions

- The generic function-call parser still contains UDF grammar actions.
  Disabled-profile guards remove parser-side UDF lookup; future parser work
  must preserve native function and stored-function resolution behavior.
- UDF class declarations remain in headers because retained SQL classes refer to
  them, but disabled-profile execution bodies must not be linked into normal
  MyLite runtime artifacts.
