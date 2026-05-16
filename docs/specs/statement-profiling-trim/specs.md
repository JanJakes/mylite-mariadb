# Statement Profiling Trim

## Problem

The default embedded profile still builds MariaDB's statement profiling
classes. Statement profiling records per-session resource-usage snapshots for
`SHOW PROFILE`, `SHOW PROFILES`, and `INFORMATION_SCHEMA.PROFILING`. This is a
server/session observability surface, not durable application state and not part
of MyLite's file-owned embedded API contract.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB documents `SHOW PROFILE` and `SHOW PROFILES` as session profiling
  statements controlled by the `profiling` and `profiling_history_size` session
  variables, with profiling information lost when the session ends:
  <https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/show/show-profile>.
- MariaDB documents `INFORMATION_SCHEMA.PROFILING` as a table containing
  statement resource-usage information similar to `SHOW PROFILE` and
  `SHOW PROFILES`:
  <https://mariadb.com/docs/server/reference/system-tables/information-schema/information-schema-tables/information-schema-profiling-table>.
- `mariadb/CMakeLists.txt` exposes the upstream `ENABLED_PROFILING` option,
  defaulting to `ON`.
- `mariadb/sql/sql_profile.h` defines the profiling classes only when
  `ENABLED_PROFILING` is defined.
- `mariadb/sql/sql_profile.cc` implements the query-profile history, status
  measurements, `SHOW PROFILE(S)` result production, and
  `INFORMATION_SCHEMA.PROFILING` fill path behind `ENABLED_PROFILING`, while
  keeping the disabled feature error and schema-field metadata available.
- `mariadb/sql/sql_parse.cc`, `mariadb/sql/sql_prepare.cc`,
  `mariadb/sql/sp_head.cc`, and `mariadb/sql/sp_instr.cc` guard profiling
  lifecycle calls behind `ENABLED_PROFILING`.
- `mariadb/sql/sys_vars.cc` registers `profiling` and
  `profiling_history_size` only when profiling is enabled, and always exposes
  read-only `have_profiling`.
- `mariadb/sql/mysqld.cc` reports `have_profiling=YES` only when
  `ENABLED_PROFILING` is defined.

## Design

- Force `ENABLED_PROFILING=OFF` in
  `cmake/mariadb-embedded-baseline.cmake`.
- Keep the upstream source layout and rely on MariaDB's existing profiling
  compile guards instead of introducing a MyLite-owned replacement object.
- Add public MyLite SQL-policy rejection for:
  - `SHOW PROFILE`,
  - `SHOW PROFILES`,
  - `SET` assignments to `profiling` and `profiling_history_size`,
  - references to `INFORMATION_SCHEMA.PROFILING`.
- Preserve ordinary `SHOW VARIABLES` and `SELECT @@have_profiling` behavior so
  applications can detect that profiling is unavailable through
  `have_profiling=NO`.

## Compatibility Impact

Statement profiling becomes explicitly unsupported. This removes a deprecated
session diagnostics feature, not SQL execution, metadata DDL, row storage, or
query planning behavior. Applications that check availability can observe
`have_profiling=NO`.

## Single-File And Embedded Lifecycle Impact

No file-format change. The trim removes per-session profiling history
allocation and lifecycle hooks from the embedded runtime. It does not add
sidecars, durable metadata, or transient companions.

## Storage-Engine Routing Impact

No storage-routing change. Profiling observes statement execution only; routed
tables, indexes, and MyLite storage remain unchanged.

## Public API Impact

No C API surface change. `mylite_exec()` and `mylite_prepare()` return stable
unsupported-surface diagnostics for statement-profiling SQL.

## Binary-Size Impact

The expected size impact is primarily archive-side, because disabling
`ENABLED_PROFILING` compiles out profiling class bodies while retaining small
schema/error stubs. The embedded build wrapper and size report will measure the
actual current effect.

## License And Dependency Impact

No new dependency and no license change. The slice uses MariaDB's existing
profiling compile option.

## Test And Verification Plan

- Add direct SQL tests for `have_profiling=NO`, rejected `SHOW PROFILE`,
  rejected `SHOW PROFILES`, rejected profiling system-variable assignment, and
  rejected `INFORMATION_SCHEMA.PROFILING` access.
- Add prepared-statement rejection tests for representative profiling SQL.
- Run default and storage-smoke MariaDB embedded builds and measurements.
- Verify `ENABLED_PROFILING:BOOL=OFF` appears in measured cache options.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` CMake build/test presets.
- Run the server-surface compatibility harness, size report, formatting, tidy,
  shell syntax checks, and diff checks.

## Acceptance Criteria

- The disabled embedded profile compiles with `ENABLED_PROFILING=OFF`.
- `have_profiling` reports `NO`.
- Direct and prepared profiling SQL is rejected with MyLite diagnostics.
- Ordinary SQL execution remains unaffected.
- Size measurements and architecture/compatibility docs are updated.

## Risks

- MariaDB registers `SHOW PROFILE(S)` grammar regardless of the compile option,
  so public MyLite policy must reject the surface before users see mixed
  MariaDB feature-disabled diagnostics.
- `INFORMATION_SCHEMA.PROFILING` metadata remains present in the source file
  even when profiling is disabled; MyLite policy must reject table access to
  keep unsupported observability surfaces explicit.
