# Status Metadata Trim

## Problem

The default MyLite embedded profile does not expose a daemon, network listener,
server account model, Performance Schema, or server monitoring subsystem, but
it still retains MariaDB's `SHOW STATUS` publication table and the dynamic
status-variable registry. That status surface describes server-global and
session counters rather than durable file-owned application state.

MyLite should keep the familiar metadata table shapes visible for probes, but
the core embedded profile should not publish inherited daemon/server status
counters until MyLite deliberately designs file-owned embedded diagnostics.

## Source Findings

Base: MariaDB 11.8.6,
`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.

- MariaDB documents `SHOW STATUS` as server status information with optional
  `GLOBAL` / `SESSION` scope and `LIKE` / `WHERE` filtering:
  <https://mariadb.com/kb/en/show-status/>.
- MariaDB documents `INFORMATION_SCHEMA.GLOBAL_STATUS` and
  `INFORMATION_SCHEMA.SESSION_STATUS` as the same status-variable information
  exposed by `SHOW GLOBAL STATUS` and `SHOW SESSION STATUS`:
  <https://mariadb.com/docs/server/reference/system-tables/information-schema/information-schema-tables/information-schema-global_status-and-session_status-tables>.
- `mariadb/sql/sql_yacc.yy` parses `SHOW [GLOBAL | SESSION] STATUS` by setting
  `SQLCOM_SHOW_STATUS`, recording the requested scope in `lex->option_type`,
  and preparing `SCH_SESSION_STATUS`.
- `mariadb/sql/sql_parse.cc` executes `SQLCOM_SHOW_STATUS` through
  `execute_show_status()`, which delegates to the prepared schema-table SELECT
  path and then restores session counters so the statement itself does not
  alter status values.
- `mariadb/sql/sql_show.cc` registers `GLOBAL_STATUS` and `SESSION_STATUS`
  schema tables with `fill_status()`. `fill_status()` reads the global dynamic
  `all_status_vars` registry and publishes rows via `show_status_array()`.
- `mariadb/sql/mysqld.cc` defines the large static `status_vars[]` publication
  array and registers it with `add_status_vars(status_vars)` during startup.
- `mariadb/sql/sql_plugin.cc` adds and removes plugin status-variable arrays
  through the same `add_status_vars()` / `remove_status_vars()` registry.

## Design

- Add `MYLITE_WITH_STATUS_METADATA`, defaulting to `ON` for upstream-style
  builds and forced `OFF` in `cmake/mariadb-embedded-baseline.cmake`.
- When disabled, compile out the large `mysqld.cc` `status_vars[]` publication
  table and its local row-format helper functions, leaving only a terminator
  symbol for code that still declares `status_vars`.
- When disabled, make `add_status_vars()`, `remove_status_vars()`,
  `init_status_vars()`, `reset_status_vars()`, and `free_status_vars()` no-ops.
  The dynamic registry stays empty, so built-in and plugin status variables are
  not published.
- Keep `GLOBAL_STATUS` and `SESSION_STATUS` information-schema table
  definitions registered so probes get stable column shapes.
- Route `GLOBAL_STATUS` and `SESSION_STATUS` to the shared MyLite empty
  schema-table filler when disabled. `SHOW STATUS`, `SHOW GLOBAL STATUS`, and
  `SHOW SESSION STATUS` therefore return empty result sets through MariaDB's
  existing SHOW-to-information-schema path.
- Do not change `SHOW VARIABLES` or system-variable metadata. Variable names,
  values, defaults, and validation remain available.

## Affected Subsystems

- MariaDB embedded build profile, `mysqld.cc` status publication, and
  `sql_show.cc` status metadata producers.
- Public direct and prepared SQL tests for server-surface metadata behavior.
- Compatibility harness labels, compatibility matrix, API docs, roadmap, and
  embedded-build size documentation.

## Compatibility Impact

MariaDB Server publishes many status rows through `SHOW STATUS` and status
information-schema tables. MyLite's default embedded profile intentionally
returns zero rows for those surfaces because they are server observability
metadata, not durable MyLite file state.

This is a compatibility tradeoff. It keeps the syntax and table shape available
for probes, but applications that depend on counters such as
`Threads_connected`, `Questions`, or `Handler_read_*` will not receive MariaDB
Server values until MyLite adds an explicit embedded diagnostics design.

## DDL Metadata Routing Impact

None. Status metadata is runtime server/session observability and does not
represent schemas, tables, indexes, constraints, or MyLite catalog records.

## Single-File And Embedded-Lifecycle Impact

No file-format change and no new companions. The disabled profile does not read
or write server status metadata side tables or monitoring state.

## Public API And File-Format Impact

No C API or `.mylite` format change. `mylite_exec()` and `mylite_prepare()`
can query `SHOW STATUS`, `SHOW GLOBAL STATUS`, `SHOW SESSION STATUS`,
`INFORMATION_SCHEMA.GLOBAL_STATUS`, and `INFORMATION_SCHEMA.SESSION_STATUS`,
and receive empty result sets or zero aggregate counts.

## Storage-Engine Routing Impact

None. Handler status counters are not published through `SHOW STATUS` in the
disabled embedded profile, but table routing, row storage, index access, and
handler behavior remain unchanged.

## Wire-Protocol Or Integration-Package Impact

Future wire-protocol or adapter layers should inherit the empty status metadata
surface unless they implement a separate embedded diagnostics layer around the
core library.

## Binary-Size Impact

The bundle-size research estimated this as a small source trim by removing the
status publication table and dynamic status-variable registry from the linked
embedded profile. The retained information-schema field metadata and SHOW
syntax keep the impact modest.

Measured on 2026-05-16:

| Profile | Archive | Size | Members | Delta From Previous Baseline |
| --- | --- | ---: | ---: | ---: |
| Default embedded | `build/mariadb-embedded/libmysqld/libmariadbd.a` | 26,902,544 bytes / 25.66 MiB | 670 | -24,144 bytes, same members |
| Storage-smoke | `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a` | 27,083,128 bytes / 25.83 MiB | 673 | -24,144 bytes, same members |

## License And Dependency Impact

No new dependency and no license change. The slice only compiles out retained
MariaDB GPL-2.0 source paths in the embedded profile.

## Test And Verification Plan

- Add direct SQL tests proving `SHOW STATUS`, `SHOW GLOBAL STATUS`, and
  `SHOW SESSION STATUS` return zero rows.
- Add direct SQL tests proving `INFORMATION_SCHEMA.GLOBAL_STATUS` and
  `INFORMATION_SCHEMA.SESSION_STATUS` remain queryable and return zero rows.
- Add prepared-statement tests proving representative status metadata queries
  prepare successfully and return no status rows.
- Run default and storage-smoke MariaDB embedded builds and measurements.
- Run `embedded-dev`, `storage-smoke-dev`, and `dev` build/test presets.
- Run the server-surface compatibility harness, size report, formatting, tidy,
  shell syntax checks, and diff checks.

## Acceptance Criteria

- The disabled embedded profile compiles with
  `MYLITE_WITH_STATUS_METADATA=OFF`.
- `SHOW STATUS`, `SHOW GLOBAL STATUS`, `SHOW SESSION STATUS`,
  `INFORMATION_SCHEMA.GLOBAL_STATUS`, and
  `INFORMATION_SCHEMA.SESSION_STATUS` remain visible with zero rows.
- Ordinary `SHOW VARIABLES`, SQL execution, table metadata, and storage-routing
  tests continue to pass.
- Size measurements and compatibility documentation are updated.

## Risks

- Some applications or health checks probe `SHOW STATUS` counters such as
  `Threads_connected`, `Uptime`, or `Questions`. Empty result sets are
  acceptable only while MyLite does not claim server monitoring compatibility.
- The registry functions become no-ops in the disabled profile, so plugin
  status variables are also absent. That is intentional for the core embedded
  profile because dynamic plugin loading and Performance Schema are already out
  of scope.
