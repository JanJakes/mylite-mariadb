# Server Surface Policy

## Problem Statement

MyLite's core API is an embedded, directory-owned database runtime. Server-owned
surfaces such as account management, dynamic plugin installation, replication,
binlog inspection, event scheduling, server help-table lookup, and statement
profiling do not fit that lifetime model. Server tuning surfaces such as query
cache management are also outside the core embedded contract. Some of these
surfaces can create durable sidecar files or system-table dependencies.
Dynamic UDF registration is the same kind of server-owned extension surface
because it loads shared libraries and persists metadata in `mysql.func`. This
slice makes those boundaries explicit and covered by tests. Later size-profile
slices also use the same policy for server file imports such as `LOAD DATA` and
`LOAD XML`.

## Source Findings

- Base ref: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` maps account, plugin, replication, binlog, and
  event and help statements to distinct command families, including
  `CREATE USER`, `GRANT`, `INSTALL PLUGIN`, `CHANGE MASTER`, `START SLAVE`,
  `SHOW BINARY LOGS`, `CREATE EVENT`, and `HELP`.
- `mariadb/sql/sql_acl.cc` contains several `--skip-grant-tables` checks, but
  embedded startup without system grant tables does not reject every account
  command before execution. MyLite must therefore gate account SQL families
  before dispatching them to MariaDB.
- `mariadb/sql/sql_plugin.cc` resolves dynamic plugin paths from `plugin_dir`.
  MyLite already points `plugin_dir` at `<db>/run/plugins`, but dynamic plugin
  installation is still server-owned behavior and should not be exposed through
  the core API.
- `mariadb/sql/sql_yacc.yy`, `mariadb/sql/sql_udf.cc`,
  `mariadb/sql/item_create.cc`, `mariadb/sql/item_func.cc`, and
  `mariadb/sql/item_sum.cc` implement dynamic UDF registration and execution.
  `CREATE FUNCTION ... SONAME` and `CREATE AGGREGATE FUNCTION ... SONAME`
  load shared libraries through the server plugin path and use the `mysql.func`
  system table.
- `mariadb/sql/sys_vars.cc` exposes `log_bin`, `skip_grant_tables`,
  `skip_networking`, and `plugin_dir`. It exposes `performance_schema` only
  when MariaDB is built with the Performance Schema storage engine. MyLite can
  prove these startup choices through SQL variables in the embedded harness.
- `mariadb/sql/sql_repl.cc` implements replication commands such as
  `CHANGE MASTER` and `START SLAVE`, which can depend on replication metadata
  files and server topology state.
- `mariadb/sql/xa.cc`, `mariadb/sql/handler.cc`, and `mariadb/sql/log.cc`
  implement external XA commands and transaction-coordinator logging. MyLite
  treats external XA as a distributed transaction-manager surface outside the
  current local embedded API, and the default embedded archive replaces the
  external-XA command runtime with fail-closed stubs.
- `mariadb/sql/events.cc` implements the event scheduler and event metadata
  paths around `mysql.event`; scheduler behavior is outside the core embedded
  profile.
- `mariadb/sql/sql_parse.cc`, `mariadb/sql/sql_profile.cc`, and
  `mariadb/sql/sys_vars.cc` wire `SHOW PROFILE`, `SHOW PROFILES`,
  `profiling`, and `profiling_history_size` behind MariaDB's
  `ENABLED_PROFILING` option. MyLite treats this as a server diagnostic
  surface and keeps `@@have_profiling=NO` in the default profile.
- `mariadb/sql/sql_cache.cc` and `mariadb/sql/sys_vars.cc` implement the server
  query cache, its `RESET QUERY CACHE` / `FLUSH QUERY CACHE` management
  commands, and `query_cache_*` variables. MyLite treats the cache as a
  server-side result-cache optimization, keeps `SQL_CACHE` and `SQL_NO_CACHE`
  as no-op SELECT hints, and keeps `@@have_query_cache=NO` in the default
  profile.
- `mariadb/sql/sql_yacc.yy`, `mariadb/sql/sql_parse.cc`,
  `mariadb/sql/sql_prepare.cc`, and `mariadb/sql/sql_load.cc` implement
  `LOAD DATA` and `LOAD XML` file-import statements. MyLite treats them as
  server filesystem or client-protocol import surfaces outside the core
  parameter-binding API.

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
   binlog administration and inspection, foreign-server metadata, dynamic UDF
   registration, SQL help-table lookup, statement profiling, query-cache
   management, and selected server-surface variables.
3. Reuse the same pre-dispatch policy boundary for optional compatibility modes
   when a size-profile slice omits their implementation. The first such case is
   Oracle SQL mode, where `sql_mode=ORACLE` is rejected while ordinary SQL modes
   and user variables named `sql_mode` remain valid.
4. Keep direct and prepared regression coverage for optional SQL helpers that a
   size-profile slice omits from the embedded function registry. The first such
   case is `SFORMAT()`, which fails predictably while ordinary `FORMAT()`
   remains available.
5. Keep direct and prepared regression coverage for omitted legacy diagnostic
   SELECT extensions. The first such case is `PROCEDURE ANALYSE()`, which fails
   predictably while ordinary SELECT queries remain available.
6. Keep direct and prepared regression coverage for omitted server utility SQL
   functions such as `BENCHMARK()`, named-lock helpers, `LOAD_FILE()`,
   replication wait/position helpers, `SLEEP()`, and `UUID_SHORT()`, while
   retained scalar functions such as `VERSION()` and `FORMAT()` remain
   available.
7. Keep direct and prepared regression coverage for omitted host-file SQL
   imports such as `LOAD DATA` and `LOAD XML`, while ordinary `INSERT`,
   prepared bindings, and `INSERT ... SELECT` remain available.

The gate is not a general SQL parser. It is a narrow first-token policy check
for statement families and explicit mode switches whose MariaDB behavior is
incompatible with the core embedded contract. Normal application DDL and DML
continue to route to MariaDB.

## Affected MariaDB Subsystems

- ACL and account-management SQL.
- Dynamic plugin loader.
- Dynamic UDF shared-library registration and execution.
- Replication and binlog command paths.
- Event scheduler and help-table command paths.
- Statement profiling command paths and variables.
- Query-cache command paths and variables.
- Performance schema startup.
- Embedded startup option handling.

## Compatibility Impact

This slice intentionally reduces the server-compatible surface of the core
library. Applications that need a MySQL/MariaDB wire-protocol server,
server-side users, replication, dynamic plugins, or dynamic shared-library UDFs
need a later integration layer around `libmylite`, not the primary
`mylite_open()` contract.

The compatibility benefit is that unsupported server behavior fails
predictably instead of partially succeeding, creating system-table dependencies,
or writing server topology sidecars.

## DDL Metadata Routing Impact

Application table DDL is unchanged. Server-owned statements for users, roles,
events, plugins, dynamic UDFs, foreign servers, and SQL help tables are
rejected before they can create or depend on `mysql.*` metadata tables.

## Database-Directory And Lifecycle Impact

The embedded runtime continues to keep durable state inside the MyLite database
directory. The server-surface policy test verifies that unsupported server
commands do not create replication metadata, binlog index files, `tc.log`,
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
- Cover startup variables for disabled binlog, performance schema, query cache,
  grant tables, networking, statement profiling, and database-local plugin
  directory.
- Cover rejected direct SQL for account, grant, event, plugin, replication, and
  binlog command families, including executable-comment wrappers that MariaDB
  treats as SQL and common syntax variants such as `CREATE OR REPLACE`,
  `CREATE DEFINER ... EVENT`, `INSTALL SONAME`, `SET SQL_LOG_BIN`, and
  `@@GLOBAL` variable assignment, `HELP`, `SHOW PROFILE`, `SHOW PROFILES`, and
  profiling variable assignment. Cover `RESET QUERY CACHE`, `FLUSH QUERY
  CACHE`, and query-cache variable assignment while keeping `SQL_CACHE` and
  `SQL_NO_CACHE` SELECT hints accepted.
- Cover rejected direct and prepared dynamic UDF registration through
  `CREATE FUNCTION ... SONAME`, including aggregate UDF syntax.
- Cover rejected direct and prepared `sql_mode=ORACLE` while keeping user
  variables named `sql_mode` accepted.
- Cover rejected direct and prepared `SFORMAT()` after the embedded size
  profile omits it.
- Cover rejected direct and prepared `PROCEDURE ANALYSE()` after the embedded
  size profile omits it.
- Cover rejected direct and prepared server utility SQL functions after the
  embedded size profile omits them, while keeping retained scalar functions
  executable.
- Cover rejected direct and prepared `LOAD DATA` and `LOAD XML` after the
  embedded size profile omits file-import runtime, while keeping ordinary
  insert paths covered.
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
  disabled statement profiling, disabled query cache, disabled grant tables,
  disabled networking, and contained plugin directory.
- Account, grant, event, plugin, replication, and binlog command families fail
  through a stable MyLite policy diagnostic.
- SQL help and statement-profiling command families fail through the same
  policy diagnostic.
- Query-cache management fails through the same policy diagnostic while
  query-cache SELECT hints remain no-op syntax.
- Dynamic UDF registration through `CREATE FUNCTION ... SONAME` and
  `CREATE AGGREGATE FUNCTION ... SONAME` fails through the same policy
  diagnostic.
- Oracle SQL mode fails through a stable MyLite policy diagnostic while normal
  SQL modes and user variables named `sql_mode` remain available.
- Optional `SFORMAT()` fails predictably in direct execution and prepared
  statements after the embedded size profile omits it.
- `PROCEDURE ANALYSE()` fails predictably in direct execution and prepared
  statements after the embedded size profile omits it.
- Server utility SQL functions fail predictably in direct execution and
  prepared statements after the embedded size profile omits them, while
  retained scalar functions remain available.
- Host-file SQL imports fail predictably in direct execution and prepared
  statements after the embedded size profile omits them, while ordinary
  inserts and prepared bindings remain available.
- MySQL/MariaDB executable comments cannot bypass the server-surface policy.
- Direct execution and prepared statements share the same policy.
- Rejected server commands do not create durable server sidecars or system-table
  directories.
- Compatibility, API, storage, and roadmap docs describe the implemented
  boundary without claiming size-profile cleanup.

## Risks And Unresolved Questions

- The policy gate is intentionally narrow. More server-owned statement families
  or optional compatibility modes may be added as later compatibility work
  exposes them.
- Size-profile slices may omit code for server surfaces or optional
  compatibility modes only after this policy coverage proves the unsupported
  runtime contract.
- A future wire-protocol integration may need different user-facing diagnostics,
  but it should still preserve the same unsupported core behavior.
