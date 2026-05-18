# Savepoint Row-DML Transactions

Status note: this slice added direct savepoint control. The later
[Prepared Savepoint Control](../prepared-savepoint-control/specs.md) slice
adds prepared `SAVEPOINT`, `ROLLBACK TO [SAVEPOINT]`, and
`RELEASE SAVEPOINT` for the same bounded file-backed transaction scope.
The later [Quoted Savepoint Names](../quoted-savepoint-names/specs.md) slice
adds backtick-quoted identifiers on the same MyLite-owned savepoint path.
The later [Handler Savepoint Hooks](../handler-savepoint-hooks/specs.md) slice
adds native MariaDB handler hooks for raw embedded routed row-DML savepoints.

## Problem

MyLite now supports bounded direct row-DML transactions through direct
`BEGIN`, `START TRANSACTION`, `COMMIT`, `ROLLBACK`, repeated transaction start,
and supported session `SET autocommit=0/1` forms. Before this slice,
savepoints were still rejected, leaving another common MySQL/MariaDB
transaction-control surface unsupported for routed `ENGINE=InnoDB` workloads.

This slice adds direct `SAVEPOINT`, `ROLLBACK TO SAVEPOINT`, and
`RELEASE SAVEPOINT` support for the same checkpoint-backed row-DML transaction
scope. It does not add transactional engine flags, handler savepoint hooks,
isolation levels, XA, transaction modifiers, or transactional DDL.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/transaction.cc:trans_savepoint()` creates or replaces a named
  savepoint in `thd->transaction->savepoints`, then calls
  `ha_savepoint(thd, newsv)`.
- `mariadb/sql/transaction.cc:savepoint_add()` replaces an older savepoint of
  the same name by calling `ha_release_savepoint()` for the old savepoint and
  removing it from the SQL-layer list before pushing the new savepoint at the
  head.
- `mariadb/sql/transaction.cc:trans_rollback_to_savepoint()` errors when the
  named savepoint is missing, calls `ha_rollback_to_savepoint()`, and then keeps
  the target savepoint as the current savepoint head, discarding later
  savepoints.
- `mariadb/sql/transaction.cc:trans_release_savepoint()` errors when the named
  savepoint is missing, calls `ha_release_savepoint()`, and then sets the
  current savepoint head to the released savepoint's predecessor, discarding the
  target and later savepoints.
- `mariadb/sql/handler.cc:ha_savepoint()` requires every registered
  transaction participant in the transaction list to provide `savepoint_set`,
  otherwise it reports unsupported `SAVEPOINT`.
- `mariadb/sql/handler.cc:ha_rollback_to_savepoint()` rolls back participants
  that existed at the savepoint and rolls back whole participants registered
  after it.
- `mariadb/sql/handler.h` documents that handlertons can request per-savepoint
  storage through `savepoint_offset` and provide `savepoint_set`,
  `savepoint_rollback`, and `savepoint_release`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innobase_savepoint()` stores
  an InnoDB undo number in the per-savepoint storage area.
- `mariadb/storage/mylite/ha_mylite.h` still advertises `HA_NO_TRANSACTIONS`.
  MyLite therefore could not honestly expose full handler-level savepoint
  support at this slice point.

## Design

Handle direct savepoint-control SQL in `libmylite` before MariaDB execution for
the bounded MyLite transaction scope:

- `SAVEPOINT name` creates a MyLite storage checkpoint frame when a direct
  transaction is active.
- Reusing an existing savepoint name marks the older matching logical savepoint
  inactive and pushes a new frame at the top. The older physical checkpoint can
  remain in the storage stack until an enclosing rollback, release, or commit
  unwinds it.
- `ROLLBACK TO [SAVEPOINT] name` rolls back every physical checkpoint frame
  created after the target, rolls back the target frame to restore its snapshot,
  then creates a fresh active frame for the same name so the target savepoint
  remains usable.
- `RELEASE SAVEPOINT name` commits and removes the target frame and every
  physical frame created after it, preserving changes while discarding the
  target and later logical savepoints.
- Full transaction `COMMIT`, `ROLLBACK`, transaction restart, close-time
  rollback, and `SET autocommit=1` unwind all active and inactive savepoint
  frames before finishing the outer transaction checkpoint.

This first savepoint slice supports simple unquoted savepoint identifiers
through the MyLite direct SQL policy parser. The later quoted-name slice adds
backtick-quoted identifier compatibility without changing the storage model.

## Affected Subsystems

- `packages/libmylite`: direct transaction-control parsing and savepoint
  checkpoint stack management.
- Embedded direct and prepared SQL tests.
- Storage-engine smoke tests for routed MyLite transaction visibility.
- API, architecture, compatibility, harness, and roadmap docs.

## Compatibility Impact

Applications can now use simple direct savepoints inside the bounded MyLite
row-DML transaction scope. Row DML before a savepoint remains visible after
`ROLLBACK TO`, row DML after that savepoint is undone, `RELEASE SAVEPOINT`
preserves changes, and full transaction rollback still restores the outer
transaction snapshot.

Compatibility remains partial:

- Savepoints are supported only through direct `libmylite` execution, not
  prepared statements.
- The first parser supports simple unquoted savepoint names.
- Handler-level savepoint hooks and fully transactional engine flags remained
  planned at this slice point.
- Transactional DDL, isolation-level changes, transaction modifiers, and XA
  remain unsupported.

## DDL Metadata Routing Impact

No catalog format changes are introduced. DDL remains rejected while a direct
transaction checkpoint is active, including when savepoints exist.

## Single-File And Embedded Lifecycle

No new durable companion files are introduced. Savepoints are nested storage
checkpoints over the same primary `.mylite` file and reuse the existing
exclusive primary-file lock held by the outer direct transaction.

## Public API And File Format

The public C API and file format do not change. Savepoints are exposed through
direct SQL execution only.

## Storage-Engine Routing Impact

The behavior applies to routed durable MyLite tables, including `ENGINE=InnoDB`
requests that resolve to MyLite. BLACKHOLE row-discard and MEMORY/HEAP
volatile-row behavior remain special cases and do not expand the durable
transaction claim.

## Wire Protocol Or Integration Impact

No wire-protocol package changes are included. A future wire-protocol wrapper
can route direct savepoint-control SQL through the same public core behavior or
through the later handler hook path, depending on which runtime entry point it
uses.

## Binary-Size And Dependency Impact

No dependency is added. The size impact is limited to a small per-handle
savepoint frame vector and direct SQL policy parsing.

## Test And Verification Plan

- Extend direct SQL policy tests to allow `SAVEPOINT`, `ROLLBACK TO`, and
  `RELEASE SAVEPOINT` inside active direct transactions and continue rejecting
  prepared savepoint-control statements.
- Add storage-smoke tests for:
  - row DML before and after a savepoint, then `ROLLBACK TO`,
  - continued use of the target savepoint after `ROLLBACK TO`,
  - `RELEASE SAVEPOINT` preserving changes,
  - duplicate savepoint names,
  - missing savepoint diagnostics,
  - full transaction rollback after savepoints,
  - close-time rollback with savepoints active,
  - continued rejection of durable DDL inside active transactions with
    savepoints.
- Run dev, embedded, storage-smoke, transaction harness groups, formatting,
  tidy, shell syntax, and whitespace checks.

## Acceptance Criteria

- Direct savepoints work over routed row-DML transactions without adding
  persistent sidecars.
- `ROLLBACK TO` restores the target savepoint snapshot and keeps the target
  savepoint active.
- `RELEASE SAVEPOINT` preserves changes while removing the target and later
  savepoints.
- Full transaction commit/rollback, restart, `SET autocommit=1`, and close
  unwind savepoint frames before finishing the outer checkpoint.
- Unsupported transaction surfaces remain explicit policy failures.

## Risks And Unresolved Questions

- Duplicate savepoint replacement can leave inactive physical checkpoint frames
  below newer frames until an enclosing unwind. This preserves LIFO correctness
  with the current storage API, but it can hold more in-memory savepoint frames
  during long transactions with many replacements.
- The later handler savepoint hook slice narrows the raw embedded gap, but
  `HA_NO_TRANSACTIONS` removal and full MariaDB transactional engine semantics
  still require broader transaction work.
