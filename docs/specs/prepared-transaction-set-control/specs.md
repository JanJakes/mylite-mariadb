# Prepared Transaction SET Control

Status note: the later
[Prepared Transaction Lifecycle Control](../prepared-transaction-lifecycle-control/specs.md)
slice accepts prepared `BEGIN`, `START TRANSACTION`, `COMMIT`, and `ROLLBACK`
forms that match the bounded direct transaction lifecycle. The later
[Prepared Parameterized Transaction SET Control](../prepared-parameterized-transaction-set-control/specs.md)
slice accepts supported single-marker prepared transaction `SET` values.

## Goal

Allow prepared statements for the transaction-related `SET` controls that
MariaDB 11.8 accepts as prepared `SQLCOM_SET_OPTION` statements and that
MyLite already supports through direct execution.

## Non-Goals

- Prepared `BEGIN`, `START TRANSACTION`, `COMMIT`, or `ROLLBACK` at this slice
  point.
- Prepared transaction controls whose value is supplied through a parameter
  marker, such as `SET autocommit=?`, at this slice point.
- Unsupported direct transaction controls: global autocommit or transaction
  variable assignments, duplicate autocommit assignments at this slice point,
  release `completion_type`, `SET STATEMENT`, duplicate `SET TRANSACTION`
  characteristics, XA, and semicolon-chained statements.
- Real MyLite storage isolation guarantees beyond the existing read-only
  enforcement mirror.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_prepare.cc:2252-2395` validates prepared statements and
  explicitly includes `SQLCOM_SET_OPTION`.
- `mariadb/sql/sql_prepare.cc:4172-4280` preserves locked plugins for prepared
  `SET` statements and finalizes the prepared statement after
  `check_prepared_statement()`.
- `mariadb/sql/sql_prepare.cc:5070-5098` executes prepared statements through
  `mysql_execute_command(thd, true)`, so prepared `SET` uses the same server
  command executor as direct `SET`.
- `mariadb/sql/sql_yacc.yy:17023-17505` parses `SET TRANSACTION`,
  scoped `SET SESSION TRANSACTION`, ordinary system-variable assignment, and
  transaction characteristics into the `SET` variable list.
- `mariadb/sql/set_var.cc:733-750` checks the entire `SET` variable list, then
  rewinds and applies updates in list order.
- `mariadb/sql/sys_vars.cc:937-941` defines `completion_type`; lines
  `4510-4568` define transaction read-only behavior; lines `4763-4828` define
  autocommit update behavior.

`BEGIN`, `COMMIT`, `ROLLBACK`, and savepoint SQL commands are not in the
MariaDB prepared-statement validation switch. MyLite already provides a
separate MyLite-owned prepared savepoint path because savepoints are storage
checkpoint controls rather than MariaDB `MYSQL_STMT` statements.

## Compatibility Impact

This removes a MyLite-only restriction for prepared session setup statements.
Applications that prepare setup SQL can use the same bounded transaction `SET`
surface as direct execution:

- `SET autocommit=0/1/DEFAULT` and supported one-autocommit `SET` lists at
  this slice point; later duplicate supported autocommit lists use the same
  prepared execution path,
- `SET completion_type=NO_CHAIN/0/DEFAULT/CHAIN/1`,
- supported duplicate `completion_type` assignment lists where the final value
  wins,
- `SET TRANSACTION` and `SET SESSION TRANSACTION` access-mode and isolation
  forms,
- supported transaction read-only and isolation variable assignments, including
  duplicate supported assignments where the final read-only value wins.

Prepared lifecycle statements remain explicit policy failures at this slice
point because MyLite's active transaction lifecycle still needs MyLite-owned
storage checkpoint transitions. A later slice accepts the bounded lifecycle
commands through the same post-execute state mirror.

## Design

During `mylite_prepare()`:

- keep the existing transaction-control classifier,
- keep MyLite-owned prepared savepoint handling unchanged,
- allow supported `SET` transaction controls to continue to MariaDB's prepared
  statement API,
- store the classified control kind on the prepared statement for post-execute
  MyLite state mirroring,
- preserve prepare-time rejection for unsupported transaction-control syntax.

During `mylite_step()` after successful `mysql_stmt_execute()` of a non-result
prepared statement:

- mirror `completion_type` defaults after MariaDB succeeds,
- apply supported transaction read-only assignment effects in SQL-list order,
- apply `SET TRANSACTION` and `SET SESSION TRANSACTION` access-mode effects,
- open or finish MyLite row-DML checkpoints for prepared
  `SET autocommit=0/1/DEFAULT`, matching the direct path,
- leave statement effects, warnings, and diagnostics on the same prepared
  execution path as other non-result prepared statements.

Parameter markers in transaction-control values remain unsupported because
MyLite must know the effective transaction-control kind before execution to
choose file-owned checkpoint behavior. Ordinary non-transaction prepared
`SET` statements are unchanged.

## File Lifecycle

No file-format, durable storage, or companion-file change. Prepared
`SET autocommit=0` opens the same MyLite transaction journal used by direct
autocommit-off row-DML transactions; prepared `SET autocommit=1/DEFAULT`
commits and removes that journal through the existing direct transaction
finish path.

## Embedded Lifecycle And API

No public C API change. The behavior is visible through existing
`mylite_prepare()`, `mylite_step()`, and `mylite_reset()`.

## Build, Size, And Dependencies

No dependency or build-profile change.

## Test Plan

- Add prepared-statement policy coverage proving supported prepared
  transaction `SET` controls now prepare and execute.
- Keep prepared `BEGIN`, `START TRANSACTION`, `COMMIT`, `ROLLBACK`,
  unsupported global controls, duplicate autocommit at this slice point,
  `SET STATEMENT`, and semicolon tails rejected at this slice point. The later
  [Autocommit Duplicate Control](../autocommit-duplicate-control/specs.md)
  slice accepts duplicate supported session autocommit assignments, and the
  later
  [Prepared Transaction Lifecycle Control](../prepared-transaction-lifecycle-control/specs.md)
  slice accepts bounded prepared lifecycle controls.
- Add storage-smoke coverage proving prepared `SET autocommit=0` opens a
  rollbackable transaction, prepared `SET autocommit=1/DEFAULT` commits it,
  prepared `SET completion_type` controls later plain completion behavior, and
  prepared transaction read-only variables enforce later write policy.
- Run dev, embedded, storage-smoke, compatibility harness, formatting, tidy,
  shell syntax, and whitespace checks.

## Acceptance Criteria

- Prepared `SET autocommit=0` followed by prepared row DML and direct
  `ROLLBACK` leaves no durable row.
- Prepared `SET autocommit=1` or `SET autocommit=DEFAULT` commits the active
  MyLite row-DML transaction.
- Prepared `SET completion_type=...` mirrors later direct `COMMIT` chaining
  behavior.
- Prepared transaction-variable assignments mirror read-only enforcement.
- Prepared transaction lifecycle statements remain explicit MyLite policy
  failures at this slice point.
