# Server Surface Policy

## Problem

MyLite's primary product shape is a local embedded library over one `.mylite`
file. Server-owned surfaces such as network listeners, account management,
binlog/replication, dynamic plugin installation, event scheduling, and
performance-schema instrumentation do not belong in the core startup contract.

This slice makes the first runtime policy explicit and testable.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/libmylite/src/database.cc` starts embedded MariaDB through
  `mysql_server_init()` with MyLite-owned arguments and `--no-defaults`.
- `mariadb/sql/sys_vars.cc` exposes `skip_networking` as a read-only command
  line variable, `log_bin` as the binary-log state variable, and
  `performance_schema` as a startup-controlled instrumentation surface. It also
  defines event scheduler state for server builds, but the embedded runtime
  does not accept the event scheduler startup variable in the current MyLite
  configuration.
- `mariadb/sql/sql_acl.cc` rejects account-management statements when
  `--skip-grant-tables` is active.
- `mariadb/sql/sql_parse.cc` routes `CREATE USER`, `GRANT`, `CREATE EVENT`,
  `INSTALL PLUGIN`, and `BINLOG` through server command paths rather than
  MyLite storage-engine table DDL.
- `mariadb/sql/sql_plugin.cc` implements dynamic plugin installation around the
  server plugin table and plugin loader, which is outside the MyLite core
  profile.

## Design

Tighten the embedded startup arguments:

- keep `--no-defaults`, `--skip-grant-tables`, and `--skip-networking`;
- add explicit `--skip-log-bin` so binlog stays off even if upstream defaults
  change;
- keep the event scheduler off by default and reject event scheduler activation
  and event DDL at the MyLite SQL policy boundary;
- add `--performance-schema=OFF` so performance schema remains disabled;
- keep the MyLite-owned empty `--plugin-dir` so dynamic plugin discovery cannot
  reach host plugin directories.

Add `compat-server-surface` coverage:

- assert disabled runtime variables through SQL;
- assert representative server SQL fails through `mylite_exec()`;
- include direct and prepared SQL file-I/O rejection through the follow-up
  file-import and SQL file-I/O policy slices;
- include storage-smoke coverage so unsupported external durable engine requests
  are also represented by the same group.

## Supported Scope

- Disabled network listener state.
- Disabled binlog state.
- Event scheduler activation and event DDL rejected by MyLite SQL policy.
- Disabled performance schema state.
- Account SQL rejection under the embedded no-grants profile.
- MyLite SQL-policy rejection for event scheduler activation, event DDL, dynamic
  plugin installation, binlog row-injection SQL, replication commands, and
  server account commands.
- MyLite SQL-policy rejection for `LOAD DATA`, `LOAD XML`, `LOAD_FILE()`,
  and `SELECT ... INTO OUTFILE` / `DUMPFILE` as follow-up explicit
  unsupported surfaces.
- MyLite SQL-policy rejection for blocking, timing, named-lock,
  replication-wait, and server-identity utility functions as follow-up
  explicit unsupported surfaces.
- MyLite SQL-policy rejection for Oracle SQL mode as a follow-up explicit
  unsupported surface.
- Existing unsupported external-engine DDL smoke grouped under the same
  compatibility surface.

## Non-Goals

- Removing all server-oriented source from the archive.
- Replacing every unsupported server SQL command with a complete SQL parser in
  this slice.
- Implementing a wire-protocol server.
- Implementing server users, grants, replication, events, plugin loading, or
  performance-schema tables in a different form.

## Compatibility Impact

Server-surface policy moves from planned to partial. MyLite remains compatible
with application SQL running inside the embedded file runtime, not with
daemon-administration SQL or server topology features.

## File-Lifecycle Impact

No new durable files are allowed. Runtime policy tests use MyLite-owned
temporary directories and the existing sidecar gates for storage-smoke coverage.

## Public API Impact

No public API changes. The policy constrains embedded runtime defaults behind
`mylite_open()` and `mylite_open_v2()`.

## Storage-Engine Routing Impact

Unsupported external durable engines remain rejected before catalog publication.
Supported application engine names such as `InnoDB`, `MyISAM`, and `Aria`
continue to route to MyLite where already covered.

## Binary-Size Impact

The original policy slice did not trim the archive directly. Follow-up
size-profile slices now use these explicit unsupported surfaces to remove
unreachable LOAD execution, host-file SQL I/O, and server utility function
paths plus the Oracle SQL mode parser from the default embedded profile.

## Test Plan

- Add embedded direct-SQL tests for disabled runtime variables.
- Add embedded direct-SQL tests for representative rejected server SQL.
- Add a `server-surface` compatibility harness group.
- Run dev, embedded, storage-smoke, format, tidy, diff, and archive-size checks.

## Acceptance Criteria

- Runtime variables report disabled network, binlog, and performance schema
  states.
- Representative server SQL fails through `mylite_exec()` with stable MyLite
  diagnostics before MariaDB execution.
- The compatibility harness exposes and runs `server-surface`.
- Roadmap and compatibility docs describe partial server-surface policy without
  claiming source trimming.

## Risks

- The SQL policy gate recognizes representative top-level server commands. A
  deeper parser-backed policy may be needed as more administrative syntax is
  found.
- Dynamic plugin code is still linked until size-profile hardening removes or
  stubs it deliberately.
