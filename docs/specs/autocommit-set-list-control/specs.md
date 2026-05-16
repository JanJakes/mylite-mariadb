# Autocommit SET-List Control

## Problem

MyLite supports direct single-assignment session `SET autocommit=0/1/DEFAULT`
forms, but still rejects a `SET` list when the list also assigns ordinary
session variables. Real clients often batch session setup, for example
`SET autocommit=0, sql_mode='ANSI'`. Rejecting those lists is stricter than
MariaDB and blocks useful transaction flows even though MyLite can mirror the
autocommit state after MariaDB accepts the full statement.

Global autocommit changes, duplicate autocommit assignments, transaction
isolation/read-only variables, release `completion_type` defaults, unsupported
`SET TRANSACTION` forms, and semicolon-chained statements remain unsupported.
Later transaction SET slices allow no-chain and chain `completion_type`
assignments in direct `SET` lists.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses `SET` as one or more option assignments in
  `option_value_list`, with separate grammar for the first listed option to
  avoid shift/reduce conflicts.
- `mariadb/sql/sql_parse.cc` executes `SQLCOM_SET_OPTION` by calling
  `sql_set_variables(thd, &lex->var_list, true)` and returns OK only after that
  call succeeds.
- `mariadb/sql/set_var.cc:sql_set_variables()` checks all variables first,
  then rewinds and updates each assignment. MyLite can therefore wait until
  MariaDB reports statement success before changing its mirrored session state.
- `mariadb/sql/sys_vars.cc:fix_autocommit()` commits when session autocommit is
  enabled, disables autocommit by setting `OPTION_NOT_AUTOCOMMIT`, and
  deliberately delays some lock cleanup for multi-assignment statements that
  evaluate expressions in the same list.
- `mariadb/sql/sys_vars.cc:Sys_autocommit` is session-only for normal `SET`
  statement use in this embedded profile; global autocommit mutates
  process-global server variables and remains outside MyLite's file-owned
  session contract.

## Design

Relax `libmylite`'s direct autocommit-control parser:

- Accept one supported session autocommit assignment anywhere in a `SET` list
  when every other assignment is not a transaction-control assignment. Later
  transaction SET slices also allow supported no-chain and chain
  `completion_type` assignments in the same direct list.
- Preserve all supported autocommit spellings:
  - `autocommit`, `SESSION autocommit`, `LOCAL autocommit`,
    `@@autocommit`, and `@@session.autocommit`,
  - `0` / `OFF` / `FALSE`,
  - `1` / `ON` / `TRUE`,
  - `DEFAULT`.
- Reject duplicate autocommit assignments in the same `SET` statement.
- Reject global autocommit assignment, `completion_type`, transaction
  isolation/read-only variables, `SET TRANSACTION`, and semicolon-chained
  statements before MariaDB execution at this slice point.
- Keep prepared autocommit-control statements rejected.

Execution reuses the existing `SetAutocommitOff` and `SetAutocommitOn`
branches. MyLite changes `autocommit_disabled` and the outer storage checkpoint
only after MariaDB accepts the whole `SET` statement.

## Affected Subsystems

- `packages/libmylite`: direct SQL transaction-control parsing.
- Direct SQL and storage-engine transaction tests.
- API, storage architecture, compatibility matrix, roadmap, and prior
  autocommit spec docs.

## Compatibility Impact

Applications can batch ordinary session setup with supported autocommit
control. MyLite still does not claim global autocommit changes, completion
defaults, isolation/read-only transaction variables, prepared autocommit
control, XA, or transactional DDL.

## DDL Metadata Routing Impact

No catalog format or DDL routing behavior changes. DDL remains rejected while a
direct transaction checkpoint is active.

## Single-File And Embedded Lifecycle

No new durable or transient file is introduced. Multi-assignment autocommit
control uses the same direct transaction checkpoint and transaction journal as
the existing single-assignment path.

## Public API And File Format

The public C API and primary `.mylite` file format do not change.

## Storage-Engine Routing Impact

The behavior applies to durable MyLite-routed row storage, including
`ENGINE=InnoDB` requests that resolve to MyLite. It does not change
BLACKHOLE or MEMORY/HEAP special behavior.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. Future protocol adapters should
delegate these supported `SET` lists to the core direct execution path.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact is limited to parser policy and
tests.

## Test And Verification Plan

- Extend direct SQL policy tests to accept supported session autocommit
  assignments mixed with ordinary session/user-variable assignments.
- Keep duplicate, global, transaction-variable, prepared, and semicolon-chained
  forms rejected with transaction-control diagnostics at this slice point.
- Add storage-smoke coverage proving:
  - `SET autocommit=0, sql_mode='ANSI'` opens a rollbackable row-DML
    transaction,
  - `SET sql_mode='', autocommit=1` commits an active autocommit-disabled
    transaction,
  - `SET sql_mode='', autocommit=DEFAULT` commits through the default-on path.
- Run dev, embedded, storage-smoke, transaction/direct-SQL/prepared-statement
  harness groups, formatting, tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- One supported session autocommit assignment may appear anywhere in a direct
  `SET` assignment list.
- Other ordinary assignments in the same list are left to MariaDB's normal
  validation and execution.
- MyLite mirrors autocommit state only after MariaDB reports success.
- Global, duplicate, unsupported transaction-variable, prepared, and
  chained-statement forms remain explicit unsupported transaction-control
  surfaces at this slice point.
- Docs and compatibility tables describe multi-assignment support without
  claiming global autocommit or isolation semantics.

## Risks And Unresolved Questions

- MyLite still uses lightweight SQL scanning rather than MariaDB's parsed
  `set_var` list. The scanner is deliberately conservative: suspicious
  transaction-control targets are rejected rather than partially interpreted.
- Supporting release `completion_type` defaults requires an embedded
  handle-lifecycle decision.
- Read-only and isolation variables need a broader storage and concurrency
  design before they can be supported honestly.
