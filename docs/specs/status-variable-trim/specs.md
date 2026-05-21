# Status Variable Trim

## Goal

Omit MariaDB server status-variable publication from the default embedded
archive while leaving ordinary SQL execution, diagnostics, warnings, result
metadata, and native storage behavior intact.

## Non-Goals

- Do not remove `SHOW VARIABLES`, system-variable validation, or
  `INFORMATION_SCHEMA.*_VARIABLES`.
- Do not remove SQL diagnostics, warnings, affected-row counts, insert ids, or
  `libmylite` error APIs.
- Do not remove application SQL functionality such as JSON, GEOMETRY/GIS,
  collations, DDL, DML, prepared statements, or native storage engines.
- Do not change normal MariaDB server builds; this is a MyLite embedded profile
  option.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/mysqld.cc` defines the large `com_status_vars[]` and
  `status_vars[]` publication arrays and registers them with
  `add_status_vars(status_vars)` during startup.
- `mariadb/sql/sql_show.cc` owns the dynamic `all_status_vars` registry,
  plugin add/remove helpers, reset/free helpers, and `fill_status()` for
  `SHOW STATUS`, `INFORMATION_SCHEMA.GLOBAL_STATUS`, and
  `INFORMATION_SCHEMA.SESSION_STATUS`.
- Performance Schema is already omitted from the default embedded profile, so
  status-variable publication only feeds server diagnostics in that profile.
- Server status counters are diagnostic observations of a daemon-style server.
  They are not required to execute application SQL against the local database
  directory.

## Compatibility Impact

`SHOW STATUS` and `INFORMATION_SCHEMA.GLOBAL_STATUS` /
`INFORMATION_SCHEMA.SESSION_STATUS` return empty result sets in the default
embedded profile. This removes server status diagnostics only. SQL errors,
SQLSTATE, warning counts, warning lookup, affected rows, insert ids, result
columns, prepared statements, and native engine behavior remain available.

`docs/COMPATIBILITY.md` records status-variable publication as an unsupported
server diagnostic surface.

## Design

Add `MYLITE_WITH_STATUS_VARIABLES`, defaulting to `ON` for normal MariaDB
embedded builds and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.

When the option is off:

- `com_status_vars[]` and `status_vars[]` compile to terminator-only arrays.
- The dynamic status-variable registry functions in `sql_show.cc` become
  no-ops.
- `fill_status()` succeeds without publishing rows, so direct and prepared
  `SHOW STATUS` keep stable query shape without exposing daemon counters.

If Performance Schema statement instrumentation is enabled in a custom build,
`com_status_vars[]` remains available for statement-name initialization.

## File Lifecycle

No database-directory, native-engine file, temporary file, lock, recovery, or
cleanup behavior changes. This slice removes in-memory server diagnostic
publication only.

## Embedded Lifecycle And API

Startup still calls the retained status helper names; the disabled profile
keeps those helpers as no-ops to minimize fork surface. The public `libmylite`
API and ordinary MariaDB diagnostics are unchanged.

## Build, Size, And Dependencies

The measured embedded archive is 27,005,960 bytes / 25.75 MiB with 705
members. This is 33,200 bytes smaller than the previous digest-trimmed archive
with no member-count change.

No dependency or license impact.

## Test Plan

- Confirm `MYLITE_WITH_STATUS_VARIABLES=OFF` appears in the embedded CMake
  cache.
- Confirm `SHOW STATUS LIKE 'Questions'` returns no rows through direct and
  prepared execution.
- Confirm `INFORMATION_SCHEMA.GLOBAL_STATUS` and
  `INFORMATION_SCHEMA.SESSION_STATUS` return no rows.
- Run the full embedded and dev CTest presets, format check, tidy, and archive
  measurement.

## Acceptance Criteria

- The embedded archive compiles with status-variable publication disabled.
- Status-variable SQL has stable empty result behavior in policy coverage.
- Ordinary SQL execution, diagnostics, prepared statements, storage tests, and
  server-surface policy tests pass.
- Build documentation records the measured archive size and compatibility
  rationale.

## Risks And Open Questions

- Clients that use `SHOW STATUS` for diagnostics will not receive MariaDB
  counters in the default embedded profile. They should use application-level
  metrics or future MyLite-specific diagnostics instead.
- Removing more status-related diagnostic code, such as process-list or
  thread-pool status helpers, should be separate source-reviewed slices.
