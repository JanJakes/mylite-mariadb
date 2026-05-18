# Double-Quoted Savepoint Names

## Goal

Support SQL-mode-aware double-quoted savepoint identifiers for direct and
prepared MyLite savepoint-control statements. When the session has
`ANSI_QUOTES` enabled, `SAVEPOINT "name"`, `ROLLBACK TO [SAVEPOINT] "name"`,
and `RELEASE SAVEPOINT "name"` should use the same MyLite-owned checkpoint path
as simple and backtick-quoted savepoint names.

## Non-Goals

- Handler-level MariaDB savepoint hooks.
- SQL-mode parsing for unrelated SQL surfaces.
- Double-quoted string handling outside savepoint-control classification.
- Transactional DDL, XA, consistent snapshots, release completion, or fully
  transactional handler flags.
- MEMORY/HEAP row savepoints are covered by the later
  [Volatile Row Transaction Snapshots](../volatile-row-transaction-snapshots/specs.md)
  slice.
- Changing savepoint-name collation or case-folding behavior.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:18319-18338` parses `ROLLBACK ... TO
  [SAVEPOINT] ident`, `SAVEPOINT ident`, and `RELEASE SAVEPOINT ident`.
- `mariadb/sql/sql_yacc.yy:15995-16014` allows both `IDENT` and
  `IDENT_QUOTED` where an `ident` is accepted.
- `mariadb/sql/sql_lex.cc:2369-2378` switches `"` from string delimiter to
  identifier delimiter when `MODE_ANSI_QUOTES` is active.
- `mariadb/sql/sql_lex.cc:2933-2983` scans delimited identifiers by treating a
  doubled delimiter as an escaped delimiter and returns `IDENT_QUOTED`.
- `mariadb/sql/sys_vars.cc:3941-3974` expands composite SQL modes such as
  `ANSI`, `ORACLE`, `MSSQL`, `POSTGRESQL`, `DB2`, and `MAXDB` to include
  `MODE_ANSI_QUOTES`.
- `mariadb/sql/sys_vars.cc:3998-4006` mirrors `MODE_ANSI_QUOTES` into
  `THD::server_status` using `SERVER_STATUS_ANSI_QUOTES`.
- `mariadb/include/mysql.h:283` exposes `MYSQL::server_status` through the
  embedded client handle, and `mariadb/include/mysql_com.h:407` defines
  `SERVER_STATUS_ANSI_QUOTES`.

## Compatibility Impact

This closes the remaining quoted-savepoint-name gap for the current bounded
file-backed row-DML transaction path. Direct SQL follows the session SQL mode at
execution time. Prepared savepoint-control statements classify and decode the
name at prepare time, matching MariaDB's parse-time treatment of prepared SQL.

Compatibility remains partial because savepoint execution is still a
`libmylite` checkpoint feature rather than a MariaDB handler-level transaction
feature.

## Design

Use MariaDB's session status bit instead of parsing `SET sql_mode` text in
MyLite. `libmylite` reads `MYSQL::server_status &
SERVER_STATUS_ANSI_QUOTES` before classifying direct SQL and before preparing
MyLite-owned savepoint-control statements.

The existing savepoint-name parser is extended as follows:

- Backtick-delimited identifiers keep the existing behavior.
- Double-quoted identifiers are accepted only when the session status has
  `SERVER_STATUS_ANSI_QUOTES`.
- Doubled delimiters decode to one literal delimiter in the stored savepoint
  name.
- Empty delimited identifiers, unterminated delimiters, chained statements, and
  unsupported transaction forms remain rejected before MariaDB execution.

`unsupported_sql_surface_message()` no longer performs a second transaction
classification pass. The public direct and prepared entry points already
classify transaction control with the correct session SQL-mode context before
checking other unsupported SQL surfaces.

## File Lifecycle

No file-format or companion-file behavior changes. Double-quoted savepoint
names reuse the existing nested storage checkpoint stack and rollback journal
lifecycle.

## Embedded Lifecycle And API

No public C API changes are required. The behavior is visible through existing
direct SQL execution and prepared-statement APIs. Session SQL-mode changes made
through MariaDB update `MYSQL::server_status`, which `libmylite` observes on
the same database handle.

## Build, Size, And Dependencies

No dependency is added. The binary-size impact is limited to a small parser
helper and tests.

## Test Plan

- Direct SQL policy tests reject double-quoted savepoint names in the default
  SQL mode and accept them with `ANSI_QUOTES`.
- Prepared statement policy tests reject double-quoted savepoint names in the
  default SQL mode and accept them when prepared under `ANSI_QUOTES`.
- Storage-engine transaction tests prove direct double-quoted rollback/release
  over a routed durable MyLite table.
- Storage-engine transaction tests prove prepared double-quoted
  rollback/release over a routed durable MyLite table, including prepare-time
  SQL-mode capture.
- Run dev, embedded, storage-smoke, compatibility harness, formatting, tidy,
  shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct and prepared double-quoted savepoint names work inside active bounded
  file-backed row-DML transactions when `ANSI_QUOTES` is active.
- The same double-quoted forms remain rejected in the default SQL mode.
- Escaped doubled quotes compare as part of the stored logical savepoint name.
- Docs and compatibility tables no longer list SQL-mode-sensitive
  double-quoted savepoint names as planned.

## Risks And Open Questions

- Savepoint-name lookup remains byte-for-byte at this slice point. The later
  [Case-Insensitive Savepoint Names](../case-insensitive-savepoint-names/specs.md)
  slice aligns lookup with MariaDB's savepoint identifier comparison.
- The later [Handler Savepoint Hooks](../handler-savepoint-hooks/specs.md)
  slice adds raw embedded handler hooks for routed durable row-DML, while full
  transactional engine semantics remain broader work.
