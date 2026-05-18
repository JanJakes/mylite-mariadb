# Volatile Row Statement Snapshots

## Problem

MyLite-routed `ENGINE=MEMORY` and `ENGINE=HEAP` tables now have volatile
transaction and savepoint snapshots. A sibling gap remains at the MariaDB
statement boundary: the handler statement transaction hook owns a durable
storage checkpoint, but it does not yet own a volatile row snapshot. Failed
autocommit statements that publish one MEMORY/HEAP row before hitting a later
duplicate-key or similar error can therefore leave process-local volatile rows
visible after MariaDB rolls the statement back.

This is separate from full SQL transaction support. The slice only aligns
runtime-volatile rows with the statement rollback boundary MyLite already uses
for durable routed rows.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.cc:trans_register_ha()` documents that storage engines
  normally register from `handler::external_lock()` and that registration is
  idempotent.
- `mariadb/sql/transaction.cc:trans_commit_stmt()` and
  `trans_rollback_stmt()` drive statement success and failure through
  `ha_commit_trans(thd, FALSE)` and `ha_rollback_trans(thd, FALSE)`.
- `mariadb/sql/handler.cc:commit_one_phase_2()` and the rollback path call the
  registered handlerton `commit(thd, all)` and `rollback(thd, all)` callbacks.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::external_lock()` registers
  the MyLite handlerton for statement transactions and starts a MyLite storage
  statement checkpoint.
- `mariadb/storage/mylite/mylite_volatile_rows.cc` now exposes
  `mylite_volatile_begin_snapshot()`, `mylite_volatile_commit_snapshot()`, and
  `mylite_volatile_rollback_snapshot()` for transaction/savepoint snapshots.
- User temporary tables use the same volatile store but are marked outside
  snapshots, preserving existing temporary-table rollback behavior.

## Design

Extend the per-connection MyLite transaction context with an optional
statement-level volatile snapshot. `ha_mylite::external_lock()` continues to
start durable statement checkpoints where possible, but it also starts one
volatile snapshot for the statement. The handlerton `commit(thd, false)` path
commits both the durable statement checkpoint and the volatile snapshot. The
`rollback(thd, false)` path rolls both back.

The durable checkpoint remains suppressed when an outer MyLite storage
checkpoint is already active. The volatile statement snapshot may still be
opened because raw embedded SQL transactions do not have the public
`libmylite` statement wrapper. Duplicate snapshot opens are guarded by the
per-connection statement snapshot pointer.

The existing volatile snapshot participation flag keeps user temporary tables
out of statement snapshots while including routed MEMORY/HEAP tables.

## Supported Scope

- Failed autocommit `INSERT` statements over routed MEMORY/HEAP tables roll
  back already-written volatile rows.
- Failed raw embedded MariaDB statements over routed MEMORY tables roll back
  already-written volatile rows through the handler statement hook.
- Existing temporary table row and DDL lifecycle remains outside volatile
  snapshots.
- Existing durable statement rollback behavior is preserved.

## Non-Goals

- Full handler-level multi-statement transaction rollback for durable rows.
- Removing `HA_NO_TRANSACTIONS`.
- Native MEMORY hash-index defaults, memory accounting, BLOB/TEXT columns, or
  broader native MEMORY/HEAP parity.
- Crash recovery for volatile row contents.
- Exhaustive failed-DML matrices beyond representative duplicate-key rollback.

## Compatibility Impact

Routed MEMORY/HEAP rows align with MariaDB's statement transaction boundary for
representative failed statements. Compatibility remains partial because the
handler still advertises non-transactional flags and broader native MEMORY/HEAP
semantics remain planned.

## File-Lifecycle Impact

No durable files or companions are introduced. Statement snapshots are
process-local memory copies and are released through the same handlerton
statement commit/rollback and connection cleanup paths that already own durable
statement checkpoints.

## Test And Verification Plan

- Extend storage-engine smoke coverage with failed multi-row duplicate-key
  statements over routed MEMORY and HEAP tables, proving no failed row remains.
- Extend raw embedded handler smoke coverage with a failed duplicate-key
  MEMORY statement, proving rollback through the MariaDB statement hook.
- Keep temporary-table transaction/savepoint coverage passing.
- Run the focused storage-engine and handler savepoint tests plus the
  transaction-hooks and storage-engine harness groups.

## Acceptance Criteria

- `mylite_commit(thd, false)` commits any active volatile statement snapshot.
- `mylite_rollback(thd, false)` rolls back any active volatile statement
  snapshot.
- Failed representative MEMORY/HEAP statements leave no partially written
  volatile rows.
- User temporary table rows remain outside volatile statement rollback.
- Docs and compatibility harness descriptions distinguish statement snapshots
  from transaction/savepoint snapshots.
