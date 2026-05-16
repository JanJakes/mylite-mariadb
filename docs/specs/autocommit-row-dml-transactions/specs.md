# Autocommit Row-DML Transactions

Status note: the later
[Transaction Restart Control](../transaction-restart-control/specs.md) slice
aligns `BEGIN` / `START TRANSACTION` inside an active autocommit-disabled
transaction with MariaDB's commit-current/start-new behavior for the same
bounded checkpoint scope. The later
[Savepoint Row-DML Transactions](../savepoint-row-dml-transactions/specs.md)
slice adds direct savepoints for the same bounded row-DML transaction scope.
The later
[Autocommit Default Control](../autocommit-default-control/specs.md) slice
maps supported direct session `SET autocommit=DEFAULT` forms to the same
autocommit-on commit path, matching MariaDB 11.8.6's `DEFAULT(TRUE)`
definition.
The later
[Autocommit SET-List Control](../autocommit-set-list-control/specs.md) slice
allows one supported session autocommit assignment inside a direct `SET` list
with ordinary non-transaction assignments, while keeping global, duplicate, and
transaction-variable forms unsupported.

## Problem

MyLite now supports direct `BEGIN`, `COMMIT`, and `ROLLBACK` for bounded
row-DML transactions over routed MyLite tables, but many MySQL/MariaDB clients
use `SET autocommit=0` rather than explicit `BEGIN`. Keeping that form rejected
blocks common application transaction flows even though the storage checkpoint
model can support the same row-DML scope.

This slice adds direct `SET autocommit=0` / `SET autocommit=1` support for the
same bounded transaction surface. It does not add isolation-level changes,
transaction modifiers, XA, transactional DDL, or transactional engine flags.
Savepoints were deferred to the later savepoint slice.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sys_vars.cc:fix_autocommit()` commits the current statement and
  transaction when autocommit is changed from disabled to enabled, then clears
  `OPTION_NOT_AUTOCOMMIT`.
- `mariadb/sql/sys_vars.cc:fix_autocommit()` sets `OPTION_NOT_AUTOCOMMIT` and
  clears `SERVER_STATUS_AUTOCOMMIT` when autocommit is changed from enabled to
  disabled.
- `mariadb/sql/sys_vars.cc:Sys_autocommit` documents the SQL behavior:
  statements commit immediately when autocommit is `1`; when it is `0`, changes
  commit on `COMMIT` or roll back on `ROLLBACK`; changing back to `1` commits
  open transactions immediately.
- `mariadb/sql/transaction.cc:trans_commit()` and
  `mariadb/sql/transaction.cc:trans_rollback()` dispatch whole-transaction
  boundaries through `ha_commit_trans(thd, TRUE)` and
  `ha_rollback_trans(thd, TRUE)`.
- `mariadb/sql/transaction.cc:trans_begin()` commits an existing
  multi-statement transaction before starting a new explicit transaction.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innobase_register_trx()`
  registers InnoDB in the normal transaction list when
  `OPTION_NOT_AUTOCOMMIT` or `OPTION_BEGIN` is set.
- `mariadb/storage/mylite/ha_mylite.h` still advertises
  `HA_NO_TRANSACTIONS`, so MyLite must keep this as a narrow `libmylite`
  checkpoint-backed public surface rather than full transactional engine
  support.

## Design

Extend the direct `libmylite` transaction-control policy:

- Allow plain direct session autocommit assignments:
  - `SET autocommit=0`
  - `SET autocommit=1`
  - `SET SESSION autocommit=0`
  - `SET SESSION autocommit=1`
  - `SET @@session.autocommit=0`
  - `SET @@session.autocommit=1`
- Treat `OFF` / `ON` and `FALSE` / `TRUE` as aliases for `0` / `1`.
- Keep prepared autocommit-control statements rejected.
- At this slice point, keep multi-assignment `SET` statements that include
  autocommit rejected, because MariaDB can evaluate expressions in the same
  statement and commits at statement end. The later
  [Autocommit SET-List Control](../autocommit-set-list-control/specs.md) slice
  relaxes this for direct `SET` lists with one supported session autocommit
  assignment and only ordinary non-transaction assignments.
- Keep global autocommit changes, `SET TRANSACTION`, isolation-level changes,
  transaction modifiers, and XA rejected. Savepoints were deferred to the later
  savepoint slice.

`libmylite` owns a new `autocommit_disabled` session flag. When direct
`SET autocommit=0` succeeds in MariaDB, `libmylite` opens an outer MyLite
storage checkpoint if one is not already active and marks the session as
autocommit-disabled. Direct and prepared row-DML statements then use nested
statement checkpoints through the existing direct transaction path.

When direct `COMMIT` or `ROLLBACK` succeeds while `autocommit_disabled` is set,
`libmylite` commits or rolls back the current outer checkpoint, then opens a
new outer checkpoint to match MariaDB's continuing non-autocommit session mode.
When direct `SET autocommit=1` succeeds while `autocommit_disabled` is set,
`libmylite` commits the current outer checkpoint and clears the mode flag.

If `SET autocommit=0` is issued during an explicit direct transaction, MyLite
keeps the existing outer checkpoint and changes only the session mode flag.
`START TRANSACTION` or `BEGIN` while that checkpoint remains active stays
unsupported in this slice instead of trying to emulate MariaDB's implicit
commit-and-new-transaction behavior.

## Affected Subsystems

- `packages/libmylite`: direct SQL policy parsing, transaction state, and
  close-time cleanup.
- Embedded direct and prepared SQL tests.
- Storage-engine smoke tests for routed MyLite tables.
- API, architecture, compatibility, harness, and roadmap docs.

## Compatibility Impact

Direct `SET autocommit=0` gains the same row-DML transaction behavior as direct
`BEGIN`: successful routed row DML remains pending until `COMMIT` or
`SET autocommit=1`; `ROLLBACK` undoes the pending row DML; failed direct or
prepared row-DML statements roll back only their own partial changes.

Compatibility remains partial:

- Savepoints remain unsupported.
- `START TRANSACTION` inside an active autocommit-disabled transaction remains
  unsupported.
- DDL inside an active autocommit-disabled transaction remains unsupported.
- `SET TRANSACTION`, isolation-level changes, transaction modifiers, XA, and
  global autocommit changes remain unsupported.
- MyLite still advertises `HA_NO_TRANSACTIONS`.

## DDL Metadata Routing Impact

No catalog format changes are introduced. DDL remains rejected while the
autocommit-disabled session has an active MyLite transaction checkpoint, because
MariaDB DDL has implicit-commit behavior and MyLite still lacks a
transaction-aware catalog design.

## Single-File And Embedded Lifecycle

No new durable companion files are introduced. The existing outer checkpoint
holds the primary-file exclusive lock while autocommit is disabled. Closing a
database handle with an active autocommit-disabled transaction rolls back the
outer checkpoint before closing the embedded MariaDB connection.

## Public API And File Format

The public C API and file format do not change. The new behavior is exposed
through direct SQL execution.

## Storage-Engine Routing Impact

The supported scope applies to routed durable MyLite tables, including
`ENGINE=InnoDB` requests that resolve to MyLite. BLACKHOLE row-discard and
MEMORY/HEAP volatile-row behavior remain special cases and do not expand the
durable transaction claim.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. A future wire-protocol wrapper
should either delegate autocommit mode to this public core behavior or replace
it with a fuller transaction implementation after savepoints and transactional
engine flags are implemented.

## Binary-Size And Dependency Impact

No dependency is added. The size impact is limited to a session flag, direct SQL
policy parsing, and tests.

## Test And Verification Plan

- Extend direct SQL policy tests to allow supported session autocommit forms and
  continue rejecting prepared autocommit-control statements.
- Add storage-smoke tests for:
  - `SET autocommit=0` + row DML + `ROLLBACK`,
  - `SET autocommit=0` + row DML + `COMMIT` with autocommit staying disabled,
  - `SET autocommit=1` committing the active autocommit-disabled transaction,
  - failed direct and prepared row-DML statements inside autocommit-disabled
    transactions,
  - close-time rollback while autocommit is disabled,
  - continued rejection of transaction modifiers, XA, global autocommit
    changes, multi-assignment autocommit changes at this slice point, and DDL
    inside the active transaction.
- Run dev, embedded, storage-smoke, transaction harness groups, formatting,
  tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct supported session autocommit assignments update MariaDB session state
  and MyLite transaction state consistently.
- Row DML under `SET autocommit=0` commits on `COMMIT` and
  `SET autocommit=1`.
- Row DML under `SET autocommit=0` rolls back on `ROLLBACK` and close.
- Failed row-DML statements inside autocommit-disabled mode roll back only their
  own partial writes.
- Unsupported transaction surfaces remain explicit policy failures with stable
  MyLite diagnostics.
- Docs and compatibility tables describe the partial autocommit scope without
  claiming full InnoDB transaction semantics.

## Risks And Unresolved Questions

- Holding the exclusive primary-file lock while autocommit is disabled can block
  other cooperating readers and writers for longer than explicit short
  transactions.
- Full MariaDB semantics for `START TRANSACTION` inside autocommit-disabled mode
  require implicit commit-and-new-transaction behavior, which is intentionally
  left for a later broader transaction slice.
- Multi-assignment `SET` support needed more careful expression and implicit
  commit ordering than this slice should absorb; the later Autocommit SET-List
  Control slice implements the bounded direct-session subset.
