# Process-List Metadata Trim

## Problem Statement

The default embedded profile still built MariaDB's process-list metadata
producers. `SHOW PROCESSLIST`, `SHOW FULL PROCESSLIST`, and
`INFORMATION_SCHEMA.PROCESSLIST` expose daemon-style thread and connection
introspection. MyLite's core API is an in-process, directory-owned library
without a server connection list or account-visible daemon session inventory.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

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
  builds and forced `OFF` in the MyLite embedded baseline.
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

## Directory And Lifecycle Impact

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

Measured on 2026-05-21 with `tools/mariadb-embedded-build all`:

| Profile | Archive size | Members | Delta |
| --- | ---: | ---: | ---: |
| Binlog injector root trimmed | 26,609,024 bytes / 25.38 MiB | 704 | baseline |
| Process-list metadata trimmed | 26,569,272 bytes / 25.34 MiB | 704 | -39,752 bytes |

The pre-strip archive moved from 27,180,312 bytes to 27,140,408 bytes.

## Test And Verification Plan

Run:

```sh
tools/mariadb-embedded-build all
cmake --build --preset embedded-dev
ctest --preset embedded-dev --output-on-failure
cmake --build --preset dev
ctest --preset dev --output-on-failure
cmake --build --preset embedded-dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset embedded-dev --target tidy
git diff --check
```

## Acceptance Criteria

- `MYLITE_WITH_PROCESSLIST_METADATA=OFF` appears in the embedded CMake cache.
- Direct and prepared process-list SHOW SQL is rejected with MyLite
  diagnostics.
- `INFORMATION_SCHEMA.PROCESSLIST` remains visible and returns zero rows.
- Supported SHOW and information-schema surfaces used by existing tests remain
  available.
- Size measurements and architecture/compatibility docs are updated.

## Risks

- Some applications use `SHOW PROCESSLIST` for operational troubleshooting.
  That belongs to daemon or adapter layers around MyLite, not the core
  directory-owned library.
- Returning an empty `INFORMATION_SCHEMA.PROCESSLIST` intentionally differs
  from MariaDB Server, which reports the querying session. This is less
  surprising for an embedded no-daemon runtime than exposing internal THD
  bookkeeping as if it were a server connection list.
