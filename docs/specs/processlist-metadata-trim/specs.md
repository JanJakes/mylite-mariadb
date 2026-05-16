# Process-List Metadata Trim

## Problem

The default embedded profile still builds MariaDB's process-list metadata
producers. `SHOW PROCESSLIST`, `SHOW FULL PROCESSLIST`, and
`INFORMATION_SCHEMA.PROCESSLIST` expose daemon-style thread and connection
introspection. MyLite's core API is an in-process, file-owned library without a
server connection list or account-visible daemon session inventory.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB documents `SHOW [FULL] PROCESSLIST` as showing running threads,
  optionally including complete query text, with visibility governed by the
  `PROCESS` privilege:
  <https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/show/show-processlist>.
- MariaDB documents `INFORMATION_SCHEMA.PROCESSLIST` as a table containing
  information about running threads, with additional timing/progress columns:
  <https://mariadb.com/docs/server/reference/system-tables/information-schema/information-schema-tables/information-schema-processlist-table>.
- `mariadb/sql/sql_yacc.yy` parses `SHOW [FULL] PROCESSLIST` into
  `SQLCOM_SHOW_PROCESSLIST`.
- `mariadb/sql/sql_parse.cc` dispatches `COM_PROCESS_INFO` and
  `SQLCOM_SHOW_PROCESSLIST` to `mysqld_list_processes()`.
- `mariadb/sql/sql_show.cc` implements `mysqld_list_processes()` through
  `thread_info`, `list_callback_arg`, and `server_threads.iterate()`.
- `mariadb/sql/sql_show.cc` also implements `fill_schema_processlist()` through
  a schema-table callback over `server_threads`.
- `mariadb/sql/sql_show.cc` registers the `PROCESSLIST` information-schema
  table with `Show::processlist_fields_info` and `fill_schema_processlist()`.

## Design

- Add `MYLITE_WITH_PROCESSLIST_METADATA`, defaulting to `ON` for upstream-style
  builds and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.
- When disabled, compile out the `SHOW PROCESSLIST` row producer and the
  `INFORMATION_SCHEMA.PROCESSLIST` thread-walk row producer from
  `sql_show.cc`.
- Keep `Show::processlist_fields_info` registered so ordinary metadata probes
  can see a stable table shape. The disabled fill function returns zero rows
  instead of walking process-global thread state.
- Add fail-closed dispatch branches in `sql_parse.cc` for `COM_PROCESS_INFO`
  and `SQLCOM_SHOW_PROCESSLIST` in case they reach MariaDB execution outside
  the public MyLite C API policy.
- Reject direct and prepared `SHOW PROCESSLIST` and
  `SHOW FULL PROCESSLIST` through the public MyLite SQL policy with stable
  unsupported-surface diagnostics.
- Do not reject `INFORMATION_SCHEMA.PROCESSLIST` in the public API. Returning
  an empty schema table keeps metadata probes deterministic without exposing a
  daemon/session inventory.

## Compatibility Impact

Process-list metadata becomes explicitly unsupported as an introspection
surface. This removes server thread visibility and privilege-governed session
inventory from the core embedded profile, not SQL execution, warnings,
ordinary `SHOW VARIABLES`, table metadata, or storage-engine routing.

## Single-File And Embedded Lifecycle Impact

No file-format change. The trim removes process-global thread-list reporting
from embedded execution and does not add sidecars, durable metadata, or
transient companions.

## Storage-Engine Routing Impact

No storage-routing change. Process-list metadata observes server threads only;
routed tables, indexes, MyLite catalog metadata, and row storage remain
unchanged.

## Public API Impact

No C API surface change. `mylite_exec()` and `mylite_prepare()` reject
`SHOW [FULL] PROCESSLIST` with stable MyLite diagnostics.
`INFORMATION_SCHEMA.PROCESSLIST` remains queryable and returns zero rows in the
disabled embedded profile.

## Binary-Size Impact

The measured size impact is small because retained MariaDB thread bookkeeping
and command-name helpers are still used by other code. The default embedded
archive is 40,712 bytes smaller with the same member count, and the
storage-smoke archive is also 40,712 bytes smaller with the same member count.

## License And Dependency Impact

No new dependency and no license change. The slice only compiles out retained
MariaDB GPL-2.0 source paths in the embedded profile.

## Test And Verification Plan

- Add direct SQL tests for rejected `SHOW PROCESSLIST` and
  `SHOW FULL PROCESSLIST`, including executable-comment handling.
- Add a direct SQL test proving quoted process-list text is not rejected.
- Add a direct SQL test proving `INFORMATION_SCHEMA.PROCESSLIST` remains
  queryable and returns zero rows.
- Add prepared-statement rejection tests for representative process-list SHOW
  commands.
- Run default and storage-smoke MariaDB embedded builds and measurements.
- Verify `MYLITE_WITH_PROCESSLIST_METADATA:BOOL=OFF` appears in measured cache
  options.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` CMake build/test presets.
- Run the server-surface compatibility harness, size report, formatting, tidy,
  shell syntax checks, and diff checks.

## Acceptance Criteria

- The disabled embedded profile compiles with
  `MYLITE_WITH_PROCESSLIST_METADATA=OFF`.
- Direct and prepared process-list SHOW SQL is rejected with MyLite
  diagnostics.
- `INFORMATION_SCHEMA.PROCESSLIST` remains visible and returns zero rows.
- Supported SHOW and information-schema surfaces used by existing tests remain
  available.
- Size measurements and architecture/compatibility docs are updated.

## Risks

- Some applications use `SHOW PROCESSLIST` for operational troubleshooting.
  That belongs to daemon or adapter layers around MyLite, not the core
  file-owned library.
- Returning an empty `INFORMATION_SCHEMA.PROCESSLIST` intentionally differs
  from MariaDB Server, which reports the querying session. This is less
  surprising for an embedded no-daemon runtime than exposing internal THD
  bookkeeping as if it were a server connection list.
