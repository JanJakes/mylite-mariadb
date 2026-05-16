# Autocommit Default Control

Status note: the later
[Autocommit SET-List Control](../autocommit-set-list-control/specs.md) slice
allows one supported session autocommit assignment, including `DEFAULT`, inside
a direct `SET` list with ordinary non-transaction assignments. Global,
duplicate, unsupported transaction-variable, prepared forms at this slice point,
and semicolon-chained forms remain unsupported. The later
[Prepared Transaction SET Control](../prepared-transaction-set-control/specs.md)
slice supports the bounded autocommit `SET` forms through prepared statements.

## Problem

MyLite supports direct session `SET autocommit=0/1` forms for bounded
file-backed row-DML transactions, but still rejects `SET autocommit=DEFAULT`.
That is a common MySQL/MariaDB-compatible way to return a session variable to
its configured default and should not require a separate application branch.

This slice adds direct session `SET autocommit=DEFAULT` support by mapping it
to MyLite's existing autocommit-on path. It does not add prepared autocommit
control at this slice point, multi-assignment `SET`, global autocommit changes,
`SET TRANSACTION`, isolation levels, XA, or transaction modifiers. The later
Autocommit SET-List Control slice adds the bounded direct-session `SET` list
subset.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sys_vars.cc:Sys_autocommit` defines `autocommit` with
  `DEFAULT(TRUE)`.
- `mariadb/sql/sys_vars.cc:fix_autocommit()` commits the active statement and
  transaction when session autocommit changes from disabled to enabled, then
  clears `OPTION_NOT_AUTOCOMMIT`.
- The same function disables autocommit by setting `OPTION_NOT_AUTOCOMMIT` and
  clearing `SERVER_STATUS_AUTOCOMMIT`.

Because the selected MariaDB base defaults `autocommit` to `TRUE`,
`SET autocommit=DEFAULT` is equivalent to `SET autocommit=1` for MyLite's
current direct session transaction scope.

## Design

Extend `libmylite`'s direct transaction-control parser:

- Treat `DEFAULT` as an autocommit-on value for supported session autocommit
  assignments.
- Keep existing accepted spelling forms such as `SET autocommit=DEFAULT`,
  `SET SESSION autocommit=DEFAULT`, and `SET @@session.autocommit=DEFAULT`
  on the same single-assignment path.
- Preserve rejection of global autocommit assignment, multi-assignment `SET`,
  prepared autocommit control, and semicolon-chained statements at this slice
  point. The later Autocommit SET-List Control slice relaxes only direct
  session `SET` lists with one supported autocommit assignment and ordinary
  non-transaction assignments.

Execution reuses the existing `SetAutocommitOn` branch: if an autocommit-
disabled MyLite transaction is active, commit the outer checkpoint and clear
the direct transaction state.

## Affected Subsystems

- `packages/libmylite`: direct SQL transaction-control parser.
- Embedded direct SQL and storage-engine transaction tests.
- API, storage architecture, compatibility, roadmap, and spec docs.

## Compatibility Impact

Applications can use direct `SET autocommit=DEFAULT` to commit the current
bounded MyLite row-DML transaction and return the session to autocommit-on
mode, matching the MariaDB default for this base line.

Compatibility remains partial:

- Prepared autocommit-control statements remain rejected at this slice point.
- Global and duplicate autocommit changes, unsupported transaction-variable
  `SET` lists, and prepared autocommit control remain rejected at this slice
  point.
- `SET TRANSACTION`, isolation-level changes, transaction modifiers, XA, and
  transactional DDL remain unsupported.

## DDL Metadata Routing Impact

No catalog format or DDL routing behavior changes. DDL remains rejected while a
direct transaction checkpoint is active.

## Single-File And Embedded Lifecycle

No new file lifecycle or companion file behavior is introduced. The existing
direct transaction checkpoint and transaction journal are committed through the
same autocommit-on path.

## Public API And File Format

The public C API and primary `.mylite` file format do not change.

## Storage-Engine Routing Impact

The behavior applies to durable MyLite-routed row storage, including
`ENGINE=InnoDB` requests that resolve to MyLite. It does not change
BLACKHOLE or MEMORY/HEAP special behavior.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. Future integration layers should
delegate supported autocommit syntax to `libmylite`.

## Binary-Size And Dependency Impact

No dependency is added. The size impact is limited to one parser branch and
tests.

## Test And Verification Plan

- Extend direct SQL policy tests for supported `DEFAULT` session spellings.
- Add storage-smoke coverage proving `SET autocommit=DEFAULT` commits an
  active row-DML transaction on a routed `ENGINE=InnoDB` table.
- Continue rejecting global, multi-assignment, prepared forms at this slice
  point, and semicolon-chained autocommit control.
- Run dev, embedded, storage-smoke, transaction harness, formatting, tidy,
  shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct `SET autocommit=DEFAULT` follows the same behavior as
  `SET autocommit=1`.
- The active MyLite row-DML transaction commits when autocommit returns to its
  default-on state.
- Unsupported autocommit surfaces remain explicit policy failures.
- Docs and compatibility tables no longer list `DEFAULT` as an unsupported
  autocommit form.

## Risks And Unresolved Questions

- This relies on MariaDB 11.8.6's default autocommit value being `TRUE`. If a
  future base or MyLite profile changes that default, this parser mapping must
  be revisited.
- Multi-assignment `SET` remained deliberately unsupported in this slice
  because MariaDB's evaluation order and implicit-commit behavior required a
  broader statement transaction design. The later Autocommit SET-List Control
  slice implements the bounded direct-session subset.
