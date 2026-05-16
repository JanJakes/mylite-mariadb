# Transaction Restart Control

## Problem

MyLite supports direct row-DML transactions through `BEGIN`,
`START TRANSACTION`, `COMMIT`, `ROLLBACK`, and supported session
`SET autocommit=0/1` forms, but it still rejects `BEGIN` or
`START TRANSACTION` while a direct transaction checkpoint is active. MariaDB
does not treat that as a nested transaction. It commits the current transaction
and starts a new one.

This slice aligns that bounded MyLite transaction surface with MariaDB's
restart behavior without adding savepoints, transaction modifiers, isolation
levels, XA, transactional DDL, or transactional engine flags.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/transaction.cc:trans_begin()` checks
  `thd->in_multi_stmt_transaction_mode()` and calls
  `ha_commit_trans(thd, TRUE)` before clearing transaction state and starting
  the new explicit transaction.
- `mariadb/sql/transaction.cc:trans_begin()` also commits when table-lock
  transaction mode is active.
- `mariadb/sql/transaction.cc:trans_commit()` dispatches whole-transaction
  commit through `ha_commit_trans(thd, TRUE)`.
- `mariadb/sql/transaction.cc:trans_rollback()` dispatches whole-transaction
  rollback through `ha_rollback_trans(thd, TRUE)`.
- `mariadb/sql/sys_vars.cc:fix_autocommit()` leaves `OPTION_NOT_AUTOCOMMIT`
  active when autocommit is disabled, so `START TRANSACTION` in that mode still
  follows the commit-current/start-new path.
- MyLite still advertises `HA_NO_TRANSACTIONS`, so the public behavior remains
  `libmylite` checkpoint-backed rather than full transactional engine support.

## Design

Remove the direct policy rejection for `BEGIN` / `START TRANSACTION` while
`database->transaction_active` is true. After MariaDB accepts the transaction
start statement, `libmylite` mirrors MariaDB's implicit commit by committing the
current outer MyLite storage checkpoint and then opening a new outer checkpoint.

Rules:

- Direct `BEGIN`, `BEGIN WORK`, and plain `START TRANSACTION` start a new outer
  checkpoint when no transaction is active.
- The same statements commit the active outer checkpoint and start a fresh
  checkpoint when a direct transaction is already active.
- If `autocommit_disabled` is set, it remains set. A later `COMMIT` or
  `ROLLBACK` still opens a new outer checkpoint for the continuing
  non-autocommit session mode.
- `START TRANSACTION READ WRITE`, `WITH CONSISTENT SNAPSHOT`, `BEGIN NOT
  ATOMIC`, `COMMIT AND CHAIN`, and other transaction modifiers remain rejected.

## Affected Subsystems

- `packages/libmylite`: direct transaction-control state handling.
- Embedded direct SQL policy tests.
- Storage-engine smoke tests for routed MyLite transaction visibility.
- API, architecture, compatibility, and roadmap docs.

## Compatibility Impact

Applications that start a transaction while one is already active now get
MariaDB-compatible commit-current/start-new behavior for the bounded row-DML
transaction scope. Earlier row-DML writes become committed; later row-DML
writes belong to the new transaction and can still roll back.

Compatibility remains partial: savepoints, transaction modifiers, isolation
levels, XA, transactional DDL, and fully transactional engine flags remain
unsupported.

## DDL Metadata Routing Impact

No catalog format changes are introduced. DDL remains rejected while a direct
transaction checkpoint is active.

## Single-File And Embedded Lifecycle

No new durable companion files are introduced. Restarting a transaction releases
the previous outer checkpoint and immediately opens the next one over the same
primary `.mylite` file.

## Public API And File Format

The public C API and file format do not change.

## Storage-Engine Routing Impact

The behavior applies to routed durable MyLite tables, including `ENGINE=InnoDB`
requests that resolve to MyLite.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. A future wire-protocol wrapper
should expose the same restart behavior when it delegates to the public core.

## Binary-Size And Dependency Impact

No dependency is added. The binary-size impact is negligible.

## Test And Verification Plan

- Update direct SQL policy tests so `START TRANSACTION` during an active
  transaction succeeds.
- Add storage-smoke coverage proving row DML before the restart commits, while
  row DML after the restart rolls back with the new transaction.
- Keep unsupported transaction modifiers, XA, and DDL inside active
  transactions rejected. Savepoints are covered by the later
  [Savepoint Row-DML Transactions](../savepoint-row-dml-transactions/specs.md)
  slice.
- Run dev, embedded, storage-smoke, transaction harness groups, formatting,
  tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct transaction-start statements no longer fail solely because a direct
  transaction checkpoint is active.
- Restarting a transaction commits the previous outer checkpoint and starts a
  new one.
- Rolling back after a restart preserves row DML committed by the restart and
  removes row DML performed after it.
- Existing unsupported transaction-control surfaces remain explicit failures.

## Risks And Unresolved Questions

- A storage checkpoint commit failure after MariaDB has accepted the restart can
  leave the session in an error state. This is the same rare I/O-failure class
  as direct `COMMIT` after MariaDB has committed.
- Full modifier support for `START TRANSACTION` remains blocked on isolation
  and access-mode semantics.
