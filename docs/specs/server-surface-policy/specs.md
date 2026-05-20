# Server Surface Policy

## Problem Statement

MyLite's core API is an embedded, directory-owned database runtime. Server-owned
surfaces such as account management, dynamic plugin installation, replication,
binlog inspection, and event scheduling do not fit that lifetime model and can
also create durable sidecar files or system-table dependencies. This slice makes
those boundaries explicit and covered by tests.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` maps account, plugin, replication, binlog, and
  event statements to distinct command families, including `CREATE USER`,
  `GRANT`, `INSTALL PLUGIN`, `CHANGE MASTER`, `START SLAVE`,
  `SHOW BINARY LOGS`, and `CREATE EVENT`.
- `mariadb/sql/sql_acl.cc` contains several `--skip-grant-tables` checks, but
  embedded startup without system grant tables does not reject every account
  command before execution. MyLite must therefore gate account SQL families
  before dispatching them to MariaDB.
- `mariadb/sql/sql_plugin.cc` resolves dynamic plugin paths from `plugin_dir`.
  MyLite already points `plugin_dir` at `<db>/run/plugins`, but dynamic plugin
  installation is still server-owned behavior and should not be exposed through
  the core API.
- `mariadb/sql/sys_vars.cc` exposes `log_bin`, `skip_grant_tables`,
  `skip_networking`, and `plugin_dir`. It exposes `performance_schema` only
  when MariaDB is built with the Performance Schema storage engine. MyLite can
  prove these startup choices through SQL variables in the embedded harness.
- `mariadb/sql/sql_repl.cc` implements replication commands such as
  `CHANGE MASTER` and `START SLAVE`, which can depend on replication metadata
  files and server topology state.
- `mariadb/sql/events.cc` implements the event scheduler and event metadata
  paths around `mysql.event`; scheduler behavior is outside the core embedded
  profile.

## Proposed Design

Use two layers:

1. Keep startup options serverless and directory-owned:
   - `--skip-grant-tables`,
   - `--skip-networking`,
   - `--skip-log-bin`,
   - `--skip-slave-start`,
   - `--performance-schema=OFF` when the MariaDB build exposes that option,
   - `--plugin-dir=<db>/run/plugins`.
2. Add a conservative MyLite SQL policy gate before direct execution and
   prepared-statement preparation for command families that are server-owned:
   users, roles, grants, password changes, dynamic plugins, events, replication,
   binlog administration and inspection, foreign-server metadata, and selected
   server-surface variables.

The gate is not a general SQL parser. It is a narrow first-token policy check
for statement families whose top-level MariaDB commands are incompatible with
the core embedded contract. Normal application DDL and DML continue to route to
MariaDB.

## Affected MariaDB Subsystems

- ACL and account-management SQL.
- Dynamic plugin loader.
- Replication and binlog command paths.
- Event scheduler command paths.
- Performance schema startup.
- Embedded startup option handling.

## Compatibility Impact

This slice intentionally reduces the server-compatible surface of the core
library. Applications that need a MySQL/MariaDB wire-protocol server,
server-side users, replication, or dynamic plugins need a later integration
layer around `libmylite`, not the primary `mylite_open()` contract.

The compatibility benefit is that unsupported server behavior fails
predictably instead of partially succeeding, creating system-table dependencies,
or writing server topology sidecars.

## DDL Metadata Routing Impact

Application table DDL is unchanged. Server-owned DDL for users, roles, events,
plugins, and foreign servers is rejected before it can create or depend on
`mysql.*` metadata tables.

## Database-Directory And Lifecycle Impact

The embedded runtime continues to keep durable state inside the MyLite database
directory. The server-surface policy test verifies that unsupported server
commands do not create replication metadata, binlog index files,
`performance_schema/`, or `mysql/` system-table directories under `datadir/`,
and that no file appears beside the MyLite database directory.

## Public API Impact

No new public functions are added. `mylite_exec()` and `mylite_prepare()` now
return `MYLITE_ERROR` with a stable MyLite diagnostic when a top-level
server-owned statement family is rejected by policy.

## Native Storage Impact

No native storage-engine behavior changes. Supported engine DDL and DML still
route to MariaDB native handlers.

## Wire-Protocol Or Integration Impact

The core library remains serverless. A future wire-protocol package may choose
to emulate or explicitly reject server-owned protocol commands, but it must not
weaken the core directory-owned contract.

## Binary-Size Impact

This slice adds policy checks and tests only. It explicitly avoids the later
size-profile hardening work that removes or stubs server-only MariaDB code.

## License Or Dependency Impact

No new dependencies or license changes.

## Test And Verification Plan

- Add `libmylite.embedded-server-surface-policy` under `embedded-dev`.
- Label it `compat.server-surface`, `compat.directory-boundary`, and
  `compat.query`.
- Cover startup variables for disabled binlog, performance schema, grant
  tables, networking, and database-local plugin directory.
- Cover rejected direct SQL for account, grant, event, plugin, replication, and
  binlog command families, including executable-comment wrappers that MariaDB
  treats as SQL and common syntax variants such as `CREATE OR REPLACE`,
  `CREATE DEFINER ... EVENT`, `INSTALL SONAME`, `SET SQL_LOG_BIN`, and
  `@@GLOBAL` variable assignment.
- Cover rejected prepared SQL for at least one server-owned statement family.
- Assert server-sidecar files and system-table directories are absent.
- Run:
  - `cmake --build --preset dev`
  - `ctest --preset dev --output-on-failure`
  - `cmake --build --preset embedded-dev`
  - `ctest --preset embedded-dev --output-on-failure`
  - `ctest --preset embedded-dev -L compat.server-surface --output-on-failure`
  - `cmake --build --preset embedded-dev --target format-check`
  - `cmake --build --preset dev --target tidy`
  - `cmake --build --preset embedded-dev --target tidy`
  - `git diff --check`
  - `tools/mariadb-embedded-build measure`

## Acceptance Criteria

- Startup variables prove disabled binlog, disabled performance schema,
  disabled grant tables, disabled networking, and contained plugin directory.
- Account, grant, event, plugin, replication, and binlog command families fail
  through a stable MyLite policy diagnostic.
- MySQL/MariaDB executable comments cannot bypass the server-surface policy.
- Direct execution and prepared statements share the same policy.
- Rejected server commands do not create durable server sidecars or system-table
  directories.
- Compatibility, API, storage, and roadmap docs describe the implemented
  boundary without claiming size-profile cleanup.

## Risks And Unresolved Questions

- The policy gate is intentionally narrow. More server-owned statement families
  may be added as later compatibility work exposes them.
- Performance schema code remains in the MariaDB archive until the later size
  profile hardening slice. This slice disables it at startup and tests the
  runtime contract only.
- A future wire-protocol integration may need different user-facing diagnostics,
  but it should still preserve the same unsupported core behavior.
