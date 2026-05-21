# Foreign-Server Metadata Trim

## Problem Statement

The default embedded profile still built MariaDB's `mysql.servers`
foreign-server metadata cache. That cache stores server-global remote
connection definitions for `CREATE SERVER`, `ALTER SERVER`, `DROP SERVER`, and
`SHOW CREATE SERVER`. MyLite's core profile is an in-process database-directory
runtime, not a daemon with global remote server definitions.

## Source Findings

Base source: MariaDB Server tag `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

Relevant source paths:

- `mariadb/sql/sql_servers.cc` owns a process-global `servers_cache` and
  loads rows from `mysql.servers` during `servers_init()`.
- `mariadb/sql/sql_servers.cc` implements `create_server()`,
  `alter_server()`, `drop_server()`, and `get_server_by_name()` around
  `mysql.servers` and the cache.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_SERVER`,
  `SQLCOM_ALTER_SERVER`, `SQLCOM_DROP_SERVER`, and
  `SQLCOM_SHOW_CREATE_SERVER` to those helpers.
- `mariadb/sql/sql_show.cc` implements `mysql_show_create_server()` by reading
  `FOREIGN_SERVER` metadata through `get_server_by_name()`.
- `mariadb/mysql-test/main/servers.test` covers MariaDB server behavior for
  `mysql.servers`, including `SHOW CREATE SERVER`.

## Design

- Add `MYLITE_WITH_FOREIGN_SERVER_METADATA`, defaulting to `ON` for
  upstream-style builds and forced `OFF` in the MyLite embedded baseline.
- When disabled, replace `sql_servers.cc` in the embedded SQL source list with
  `mylite_sql_servers_disabled.cc`.
- Keep `servers_init()` and `servers_reload()` as no-op success paths so
  embedded startup does not read or require `mysql.servers`.
- Reject `CREATE SERVER`, `ALTER SERVER`, and `DROP SERVER` in the stub if
  execution reaches MariaDB outside the public MyLite policy.
- Reject direct and prepared `CREATE SERVER`, `CREATE OR REPLACE SERVER`,
  `ALTER SERVER`, `DROP SERVER`, and `SHOW CREATE SERVER` through the public
  MyLite SQL policy with stable unsupported-surface diagnostics.

## Compatibility Impact

Foreign-server metadata becomes explicitly unsupported. This removes
server-global remote connection definitions, not application tables, native
storage engines, SQL parsing, DDL/DML, JSON, GEOMETRY/GIS, transactions, or
prepared statements.

## Directory And Lifecycle Impact

No file-format change. The trim prevents the embedded startup path from
loading remote server definitions from `mysql.servers` and does not add files,
sidecars, locks, or transient companions.

## Storage-Engine Routing Impact

No native storage-routing change. Foreign-server metadata supports remote
connection definitions used by server/engine integration surfaces; it is not a
table-storage engine path for MyLite-owned application tables.

## Public API Impact

No C API surface change. `mylite_exec()` and `mylite_prepare()` reject
foreign-server metadata SQL with stable MyLite diagnostics.

## Binary-Size Impact

Measured on 2026-05-21 with `tools/mariadb-embedded-build all`:

| Profile | Archive size | Members | Delta |
| --- | ---: | ---: | ---: |
| Process-list metadata trimmed | 26,569,272 bytes / 25.34 MiB | 704 | baseline |
| Foreign-server metadata trimmed | 26,553,928 bytes / 25.32 MiB | 704 | -15,344 bytes |

The pre-strip archive moved from 27,140,408 bytes to 27,124,416 bytes.

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

- `MYLITE_WITH_FOREIGN_SERVER_METADATA=OFF` appears in the embedded CMake
  cache.
- `sql_servers.cc.o` is absent from `libmariadbd.a`.
- `mylite_sql_servers_disabled.cc.o` is present in `libmariadbd.a`.
- Direct and prepared foreign-server metadata SQL is rejected with MyLite
  diagnostics.
- Supported application SQL, native storage, JSON, GEOMETRY/GIS, transactions,
  and prepared statements remain covered by the test suite.
- Size measurements and architecture/compatibility docs are updated.

## Risks

- Some server deployments use foreign-server definitions with FEDERATED-style
  remote table setups. That is a server topology surface and remains outside
  the core embedded profile.
- `SHOW CREATE SERVER` is rejected by public policy before it reaches MariaDB.
  If policy is bypassed, the disabled stub has no cache entry and the inherited
  show path reports that the named server does not exist.
