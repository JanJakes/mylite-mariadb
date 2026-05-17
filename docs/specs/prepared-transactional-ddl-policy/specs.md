# Prepared Transactional DDL Policy

Status note: the later
[Prepared Transaction Lifecycle Control](../prepared-transaction-lifecycle-control/specs.md)
slice allows prepared transaction-start and completion controls for the same
bounded row-DML transaction scope. The DDL policy now applies to active MyLite
transactions regardless of whether direct or prepared SQL started them.

## Problem

MyLite rejects direct storage DDL while a bounded direct transaction is active,
because the current transaction layer only proves row-DML rollback semantics.
Prepared statements currently classify storage DDL for statement checkpoints,
but they do not reject prepared DDL when execution happens inside an active
direct transaction. That lets the prepared API bypass the direct SQL policy.

This slice makes prepared durable storage DDL execution fail with the same
MyLite transactional-DDL diagnostic used by direct SQL. It does not add durable
transactional DDL semantics. Explicit temporary table create/drop inside active
transactions was later covered by
[Temporary DDL Transactions](../temporary-ddl-transactions/specs.md).

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB documentation for
  [`START TRANSACTION`](https://mariadb.com/docs/server/reference/sql-statements/transactions/start-transaction)
  describes transaction boundaries and implicit-commit behavior.
- `mariadb/sql/sql_parse.cc:536-556` marks `CREATE TABLE`,
  `ALTER TABLE`, `TRUNCATE`, `DROP TABLE`, `RENAME TABLE`, and related DDL
  with `CF_AUTO_COMMIT_TRANS`, so MariaDB treats them as implicit-commit
  statements rather than ordinary row-DML transaction participants.
- `mariadb/sql/sql_parse.cc:3810-3863` commits an active transaction before
  executing commands that carry `CF_IMPLICIT_COMMIT_BEGIN`, then separately
  rejects commands marked `CF_DISALLOW_IN_RO_TRANS` inside read-only
  transactions.
- `mariadb/sql/sql_parse.cc:848-854` marks core table DDL as disallowed in
  read-only transactions.
- `mariadb/sql/sql_prepare.cc:2290-2315` validates `SQLCOM_CREATE_TABLE` as a
  preparable statement.
- `mariadb/sql/sql_prepare.cc:3277-3353` routes `COM_STMT_EXECUTE` through
  `Prepared_statement::execute_loop()`.
- `mariadb/sql/sql_prepare.cc:5070-5098` executes prepared statements by
  calling `mysql_execute_command(thd, true)`, so prepared DDL follows the same
  MariaDB execution semantics after MyLite's public API policy gate.

## Design

- Keep prepared DDL preparable when it is otherwise supported by MyLite. The
  transaction policy belongs at execution time because a statement may be
  prepared before the active transaction starts.
- Store a prepared-statement flag for SQL that MyLite classifies as durable
  storage outer-checkpoint work: `CREATE`, `ALTER`, `DROP`, `RENAME`, and
  `TRUNCATE`, excluding explicit temporary table create/drop.
- In `mylite_step()`, before parameter binding and before MariaDB execution,
  reject those prepared statements when the owning handle has an active direct
  MyLite transaction.
- Use the existing stable diagnostic text:
  `unsupported transactional DDL SQL surface`.
- Keep direct SQL behavior unchanged.

## Affected Subsystems

- `packages/libmylite`: prepared statement classification and execution
  policy.
- Storage-engine smoke tests over routed MyLite tables.
- Transaction, API, storage, compatibility, and roadmap documentation.

## Compatibility Impact

MyLite becomes stricter and more internally consistent. Applications that
prepare durable DDL outside a transaction can still execute it outside a
transaction. Applications that execute prepared durable DDL after `BEGIN`,
`START TRANSACTION`, `SET autocommit=0`, or an active savepoint now receive the
same MyLite policy error that direct SQL receives.

This remains less permissive than MariaDB's implicit-commit durable DDL
behavior. That is intentional until MyLite can prove transactional DDL or
explicitly model implicit transaction boundary effects for the embedded
single-file runtime.

## DDL Metadata Routing Impact

No metadata format changes. The slice prevents prepared DDL from publishing
catalog changes inside the current bounded row-DML transaction scope.

## Single-File And Embedded Lifecycle

No file-format, companion-file, lock, or lifecycle changes. The change avoids
starting a DDL statement checkpoint while an outer direct transaction checkpoint
is active.

## Public API And File Format

No C API or `.mylite` file-format changes. `mylite_step()` returns
`MYLITE_ERROR`, SQLSTATE `HY000`, MariaDB errno `0`, and the MyLite diagnostic
for the policy rejection.

## Storage-Engine Routing Impact

The policy applies to routed durable MyLite tables, including requests such as
`ENGINE=InnoDB` that resolve to MyLite. It does not add native InnoDB
transactional DDL, implicit commit emulation, or handler-level transactional
engine flags.

## Wire Protocol Or Integration Impact

No wire-protocol package changes. Future protocol adapters that expose prepared
statements should delegate to this core behavior.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact is limited to one prepared
statement flag and a small execution check.

## Test And Verification Plan

- Add storage-smoke coverage for a prepared storage DDL statement prepared
  before an active transaction and executed inside that transaction.
- Verify the policy diagnostic has no MariaDB errno and does not depend on
  MariaDB execution.
- Run focused storage-smoke, embedded, transaction harness, formatting, tidy,
  shell syntax, and whitespace checks before commit.

## Acceptance Criteria

- Prepared durable storage DDL execution fails while a direct MyLite
  transaction is active.
- The rejection happens even when the statement was prepared before the
  transaction started.
- Prepared durable storage DDL remains allowed outside the active direct
  transaction policy.
- Docs describe direct and prepared transactional-DDL policy consistently
  without claiming transactional DDL support.

## Risks And Unresolved Questions

- The policy still uses MyLite's conservative SQL classification rather than
  MariaDB's parsed `LEX` command flags. Suspicious forms should remain rejected
  or handled by existing unsupported-surface gates until MyLite moves this
  policy closer to parsed MariaDB command state.
- Full implicit-commit durable DDL compatibility is a separate design problem
  because it would need precise storage checkpoint publication and rollback
  semantics across direct and prepared APIs.
