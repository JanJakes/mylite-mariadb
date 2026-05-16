# Foreign Server Metadata Trim

## Goal

Remove MariaDB's `mysql.servers` foreign-server metadata cache from the default
MyLite embedded profile and make the unsupported `CREATE SERVER` family explicit
through public SQL policy tests.

## Non-Goals

- Do not implement FEDERATED, FederatedX, CONNECT, Spider, or any other remote
  storage-engine integration.
- Do not add MyLite catalog storage for global foreign-server definitions.
- Do not change supported `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`,
  `ENGINE=BLACKHOLE`, or `ENGINE=MEMORY` routing.
- Do not remove ordinary server startup or cleanup paths unrelated to
  foreign-server metadata.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_servers.cc` owns the global foreign-server cache and reads
  definitions from `mysql.servers` in `servers_reload()`.
- `mariadb/sql/mysqld.cc` calls `servers_init(0)` during server initialization
  and `servers_free(1)` during shutdown. In the embedded MyLite profile this
  currently attempts to open `mysql.servers`, which is not part of MyLite's
  file-owned bootstrap state.
- `mariadb/sql/sql_reload.cc` calls `servers_reload()` as part of reload paths.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_SERVER`,
  `SQLCOM_ALTER_SERVER`, and `SQLCOM_DROP_SERVER` to `create_server()`,
  `alter_server()`, and `drop_server()`.
- `mariadb/sql/sql_show.cc` uses `get_server_by_name()` for
  `SHOW CREATE SERVER`.
- `mariadb/sql/sql_servers.h` exposes only the initialization, reload, cleanup,
  DDL, and lookup functions needed by retained SQL code.
- MariaDB documentation describes `CREATE SERVER` as creating an entry in the
  `mysql.servers` table for Spider, CONNECT, FEDERATED, and FederatedX table
  definitions: <https://mariadb.com/kb/en/create-server/>.
- MariaDB documentation describes `SHOW CREATE SERVER` as showing the statement
  that created a server definition:
  <https://mariadb.com/kb/en/show-create-server/>.

## Compatibility Impact

Foreign-server definitions are daemon/global metadata for remote storage-engine
integrations. They do not fit the current single-file embedded MyLite runtime,
and MyLite already rejects unsupported external durable engines before catalog
publication.

`CREATE SERVER`, `ALTER SERVER`, `DROP SERVER`, and `SHOW CREATE SERVER` remain
out of scope. The public MyLite SQL API should reject representative direct and
prepared forms before MariaDB execution.

## Design

- Add `MYLITE_WITH_FOREIGN_SERVER_METADATA`, defaulting to `ON` for upstream-like
  builds.
- Force `MYLITE_WITH_FOREIGN_SERVER_METADATA=OFF` from the MyLite embedded
  baseline.
- Replace `sql_servers.cc` with `mylite_sql_servers_disabled.cc` in the disabled
  profile for both the server SQL source list and the embedded SQL source list.
- Implement no-op embedded stubs:
  - `servers_init()` succeeds without reading `mysql.servers`;
  - `servers_reload()` succeeds without touching server tables;
  - `servers_free()` is a no-op;
  - `get_server_by_name()` returns `NULL`;
  - DDL entry points fail closed if reached outside the public MyLite SQL gate.
- Extend the SQL policy gate to catch `CREATE OR REPLACE SERVER` and
  `SHOW CREATE SERVER` in addition to the already blocked plain `CREATE`,
  `ALTER`, and `DROP SERVER` forms.

## File Lifecycle

No `.mylite` file-format or companion-file change is required. The trim removes
an embedded startup path that attempts to read server-owned `mysql.servers`
metadata outside the MyLite catalog.

## Embedded Lifecycle And API

Startup and shutdown remain owned by `mylite_open()` / `mylite_close()`.
`servers_init()` no longer depends on a MariaDB grant/system table, and public
direct/prepared SQL continues to return `MYLITE_ERROR` with a stable MyLite
diagnostic for unsupported server-oriented SQL.

## Build, Size, And Dependencies

The disabled profile should omit `sql_servers.cc.o` from both the default
embedded archive and the opt-in storage-smoke archive. No dependency or license
change is introduced.

Measured on 2026-05-16 after implementation:

| Profile | Archive Size | Members | Delta From Previous Profile |
| --- | ---: | ---: | ---: |
| Default embedded | 27,143,408 bytes / 25.89 MiB | 671 | -16,784 bytes, unchanged |
| Storage-smoke | 27,323,992 bytes / 26.06 MiB | 674 | -16,784 bytes, unchanged |

## Test Plan

1. Reconfigure and rebuild the default MariaDB embedded archive.
2. Reconfigure and rebuild the storage-smoke archive with
   `PLUGIN_MYLITE_SE=STATIC`.
3. Confirm both archives omit `sql_servers.cc.o` and include
   `mylite_sql_servers_disabled.cc.o`.
4. Add direct and prepared SQL policy tests for `CREATE SERVER`,
   `CREATE OR REPLACE SERVER`, `ALTER SERVER`, `DROP SERVER`, and
   `SHOW CREATE SERVER`.
5. Run affected embedded and storage-smoke CTest presets, the server-surface
   compatibility report, the size report, format, tidy, shell syntax, and diff
   checks.

## Acceptance Criteria

- `MYLITE_WITH_FOREIGN_SERVER_METADATA=OFF` is part of the committed embedded
  baseline.
- Default upstream-like builds keep `sql_servers.cc` unless explicitly disabled.
- The default and storage-smoke archives omit `sql_servers.cc.o` and include
  `mylite_sql_servers_disabled.cc.o`.
- MyLite embedded startup no longer attempts to open `mysql.servers`.
- Public direct and prepared SQL reject foreign-server metadata surfaces before
  MariaDB execution.
- Compatibility, architecture, roadmap, and size-profile docs describe the
  unsupported boundary and measured size impact.
- Relevant tests and static checks pass.

## Risks And Open Questions

- Future remote-engine compatibility would need a separate catalog-backed design
  for global server definitions. That work should remain outside the core
  single-file runtime until a concrete application need exists.
- Some retained SQL code can still ask `get_server_by_name()` about a named
  server. Returning `NULL` preserves the fail-closed behavior for unsupported
  remote-engine paths.
