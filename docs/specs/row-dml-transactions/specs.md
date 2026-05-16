# Row-DML Transactions

Status note: this slice added direct `BEGIN`, `COMMIT`, and `ROLLBACK` support
for bounded row-DML transactions. The later
[Autocommit Row-DML Transactions](../autocommit-row-dml-transactions/specs.md)
slice adds supported direct session `SET autocommit=0/1` forms for the same
checkpoint-backed scope. The later
[Transaction Restart Control](../transaction-restart-control/specs.md) slice
aligns repeated direct `BEGIN` / `START TRANSACTION` with MariaDB's
commit-current/start-new behavior.

## Problem

MyLite has statement checkpoints and MariaDB statement transaction hooks, but
public SQL transaction control is still rejected. That keeps compatibility
honest, but it blocks applications that expect ordinary `BEGIN`, `COMMIT`, and
`ROLLBACK` around row DML on `ENGINE=InnoDB` tables routed to MyLite.

This slice adds a bounded first transaction surface: direct `libmylite`
execution of row-DML transactions over routed MyLite tables. It does not claim
savepoints, `SET autocommit=0`, transactional DDL, XA, isolation levels, or
fully transactional engine flags.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/transaction.cc:trans_begin()` sets `OPTION_BEGIN` for
  `START TRANSACTION` and related transaction-start statements.
- `mariadb/sql/transaction.cc:trans_commit_stmt()` calls
  `ha_commit_trans(thd, FALSE)` for statement transaction participants at
  successful statement end.
- `mariadb/sql/transaction.cc:trans_rollback_stmt()` calls
  `ha_rollback_trans(thd, FALSE)` for failed statement cleanup.
- `mariadb/sql/transaction.cc:trans_commit()` and `trans_rollback()` call
  `ha_commit_trans(thd, TRUE)` and `ha_rollback_trans(thd, TRUE)` for the full
  transaction.
- `mariadb/sql/handler.cc:trans_register_ha()` registers handlertons in the
  statement and normal transaction lists. The function is intentionally
  idempotent.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innobase_register_trx()`
  registers InnoDB in the statement list and, when `OPTION_BEGIN` or
  `OPTION_NOT_AUTOCOMMIT` is set, in the normal transaction list.
- `mariadb/sql/handler.h` defines savepoint hooks and `HA_NO_TRANSACTIONS`.
  MyLite still advertises `HA_NO_TRANSACTIONS`, so this slice must keep the
  compatibility claim narrower than full transactional engine support.
- `mariadb/storage/mylite/ha_mylite.cc` already installs `commit` and
  `rollback` callbacks and registers statement transactions from
  `ha_mylite::external_lock()`, but the context owns only one statement
  checkpoint and ignores the `all` transaction-boundary argument.
- `packages/mylite-storage/src/storage.c` keeps a single thread-local
  statement checkpoint. That is not enough for explicit transactions because a
  failed statement inside a still-active transaction must roll back only to the
  statement start while preserving earlier transaction changes.

## Design

Add nested storage checkpoints for one active MyLite primary file per thread.
The outer checkpoint represents the explicit transaction snapshot. Each
mutating row-DML statement inside that transaction receives an inner statement
checkpoint from the `libmylite` direct or prepared execution wrapper before
MariaDB executes the statement. The MyLite handler keeps owning autocommit
row-DML statement checkpoints through MariaDB's statement transaction hook path,
and skips opening a duplicate handler checkpoint when an outer `libmylite`
checkpoint is already active.

Storage checkpoint nesting rules:

- `mylite_storage_begin_statement()` may begin a nested checkpoint only when
  the active checkpoint targets the same primary file.
- Nested checkpoints borrow the outer locked file handle instead of taking a
  second advisory lock on the same file.
- `mylite_storage_commit_statement()` and
  `mylite_storage_rollback_statement()` require LIFO checkpoint ownership.
- Rolling back an inner checkpoint restores the header and catalog snapshot
  captured at inner begin time.
- Rolling back the outer checkpoint restores the transaction-start header and
  catalog snapshot.

Direct transaction rules:

- `libmylite` begins the outer storage checkpoint after plain direct
  `BEGIN`, `BEGIN WORK`, or `START TRANSACTION` succeeds in MariaDB.
- `libmylite` commits or rolls back the outer storage checkpoint after plain
  direct `COMMIT`, `COMMIT WORK`, `ROLLBACK`, or `ROLLBACK WORK` succeeds in
  MariaDB.
- Direct and prepared row-DML statements begin a nested statement checkpoint
  while that direct transaction is active, so a failed statement rolls back
  only its own partial writes.
- Closing a database handle with an active direct transaction issues MariaDB
  `ROLLBACK` and rolls back the outer MyLite storage checkpoint before closing.

Handler transaction rules:

- `ha_mylite::external_lock()` keeps the autocommit statement-checkpoint path
  for routed row DML when no outer `libmylite` storage checkpoint is active.
- It can start a handler-owned transaction checkpoint when MariaDB has
  `OPTION_BEGIN` or `OPTION_NOT_AUTOCOMMIT` set and no direct `libmylite`
  checkpoint is already active, but MyLite still does not expose autocommit
  mode as a supported public transaction surface in this slice.
- It registers `mylite_hton` in MariaDB's statement transaction list for row
  DML and in the normal transaction list for explicit transactions.
- `mylite_commit(thd, false)` and `mylite_rollback(thd, false)` finish only the
  active statement checkpoint.
- `mylite_commit(thd, true)` and `mylite_rollback(thd, true)` finish the outer
  transaction checkpoint after any defensive statement cleanup.

Public SQL policy rules:

- Direct `mylite_exec()` allows plain `BEGIN`, `BEGIN WORK`,
  `START TRANSACTION`, `COMMIT`, `COMMIT WORK`, `ROLLBACK`, and
  `ROLLBACK WORK`.
- Direct `mylite_exec()` continues to reject `SET autocommit`,
  `SET TRANSACTION`, XA, transaction modifiers such as `COMMIT AND CHAIN`, and
  prepared transaction-control statements. Savepoints were outside this slice
  and are covered by the later
  [Savepoint Row-DML Transactions](../savepoint-row-dml-transactions/specs.md)
  slice.
- Transactional DDL remains rejected while a direct transaction is active.
  MySQL/MariaDB DDL has implicit-commit semantics, and MyLite still needs a
  catalog/metadata transaction design before allowing DDL inside explicit
  transactions.

## Affected Subsystems

- `packages/mylite-storage`: nested statement checkpoint ownership and tests.
- `mariadb/storage/mylite`: handlerton context split between statement and
  transaction checkpoints.
- `packages/libmylite`: direct SQL policy for transaction-control statements
  and active transaction tracking.
- Embedded storage-engine smoke tests, compatibility docs, and roadmap.

## Compatibility Impact

`ENGINE=InnoDB` row DML routed to MyLite gains a first explicit transaction
surface for direct execution. Earlier successful row-DML statements in a
transaction remain visible to the transaction and are undone by `ROLLBACK`;
failed direct or prepared statements inside the transaction restore only their
own partial changes; `COMMIT` keeps the transaction changes.

Compatibility remains partial:

- MyLite still advertises `HA_NO_TRANSACTIONS`.
- Savepoints and `SET autocommit=0` remain unsupported.
- Transactional DDL remains unsupported.
- Prepared transaction-control statements remain unsupported.
- Isolation levels and concurrent writer behavior are not claimed.

## DDL Metadata Routing Impact

No catalog format changes are introduced. Table and schema DDL continue to use
outer statement checkpoints outside explicit transactions. When a direct
transaction is active, DDL is rejected before MariaDB execution so the MyLite
catalog cannot diverge from MariaDB's implicit-commit behavior.

## Single-File And Embedded Lifecycle

No new durable companion files are introduced. The existing rollback journal
still protects physical publication. Explicit transactions hold the existing
primary-file exclusive lock while a transaction checkpoint is active. Closing a
database handle with an active transaction rolls it back before closing the
MariaDB connection.

## Public API And File Format

The public C API does not change. Transaction control is exposed through direct
SQL execution. The file format does not change because checkpoints restore
existing header and catalog pages.

## Storage-Engine Routing Impact

All durable routed engines handled by `ha_mylite` share the same transaction
behavior. BLACKHOLE row-discard and MEMORY/HEAP volatile-row behavior remain
non-durable special cases and do not expand this transaction claim.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. A future wire-protocol wrapper
should drive the same direct SQL transaction policy through the public core or
replace it with a fuller transaction-capable protocol layer after savepoints
and autocommit mode are implemented.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact is limited to MyLite storage
checkpoint stack state, handler transaction glue, SQL policy parsing, and test
coverage.

## Test And Verification Plan

- Add storage tests for nested checkpoint commit/rollback and LIFO misuse.
- Add storage-smoke tests for:
  - `BEGIN` + row DML + `COMMIT`,
  - `BEGIN` + row DML + `ROLLBACK`,
  - a failed unique-key statement inside a transaction rolling back only the
    failed statement,
  - close-time rollback of an active transaction,
  - continued rejection of savepoints, autocommit mode changes, transaction
    modifiers, and DDL inside an active transaction.
- Keep prepared transaction-control rejection coverage.
- Run storage, embedded, storage-smoke, transaction harness groups, formatting,
  tidy, and whitespace checks.

## Acceptance Criteria

- Nested storage checkpoints commit and roll back in LIFO order over the same
  primary file.
- `BEGIN`/`COMMIT` direct execution preserves row-DML changes on routed MyLite
  tables.
- `BEGIN`/`ROLLBACK` direct execution removes row-DML changes from routed
  MyLite tables.
- Failed row-DML statements inside an explicit transaction roll back only their
  own partial changes while preserving earlier transaction changes.
- Savepoints, `SET autocommit=0`, XA, prepared transaction control, transaction
  modifiers, and transactional DDL remain explicitly unsupported.
- Docs and compatibility tables describe the partial transaction scope without
  claiming full InnoDB semantics.

## Risks And Unresolved Questions

- Holding the exclusive primary-file lock for a transaction is conservative and
  blocks concurrent readers/writers. Full multi-writer transaction locking
  remains a later roadmap slice.
- The checkpoint model restores header/catalog reachability, but it is not a
  final WAL or logical undo design.
- `HA_NO_TRANSACTIONS` remains until savepoints, autocommit mode, transaction
  metadata, and broader rollback semantics are implemented and tested.
