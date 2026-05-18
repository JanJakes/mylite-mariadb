# Volatile Row Transaction Snapshots

## Problem

Routed `ENGINE=MEMORY` and `ENGINE=HEAP` tables already store durable metadata
in the primary `.mylite` file while keeping row and supported index contents in
process-local volatile MyLite state. Before this slice, durable MyLite rows
participated in direct/prepared transactions and savepoints, but runtime
volatile rows did not. Rolling back a transaction or savepoint could therefore
restore durable routed tables while leaving MEMORY/HEAP rows visible.

User temporary tables use the same volatile row store, but MariaDB keeps
temporary table rows and temporary create/drop lifecycle outside ordinary
transaction rollback. The snapshot design must not accidentally make user
temporary tables transactional.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.h` exposes handlerton transaction and savepoint hooks
  through `commit`, `rollback`, `savepoint_set`, `savepoint_rollback`, and
  `savepoint_release`.
- `mariadb/sql/handler.cc:trans_register_ha()` registers storage engines for
  statement and normal transaction boundaries.
- `mariadb/storage/heap/ha_heap.cc` is the native MEMORY/HEAP handler family
  and does not use durable table files for row storage.
- `mariadb/storage/mylite/ha_mylite.cc` routes explicit `MEMORY` and `HEAP`
  table metadata to MyLite while setting `volatile_rows` for process-local row
  storage.
- `packages/libmylite/src/database.cc` owns the public direct/prepared
  transaction and savepoint guards for the current bounded row-DML scope.
- User temporary table DDL and row-DML tests already assert temporary rows and
  temporary create/drop lifecycle survive ordinary transaction rollback.

## Design

Add a MyLite volatile-row snapshot API that copies runtime volatile tables and
aliases for one primary `.mylite` file. Rollback replaces only snapshot
participating tables for that primary file with the copied state. Commit frees
the copy.

Each volatile table records whether it participates in transaction snapshots:

- routed `ENGINE=MEMORY` and `ENGINE=HEAP` tables participate;
- user temporary tables do not participate, even though they use the same
  process-local row store.

Rollback preserves MEMORY/HEAP autoincrement gaps by keeping the larger of the
snapshot counter and the current counter for each restored table. This mirrors
the existing MyLite durable-row behavior where row visibility rolls back while
generated autoincrement reservations are not reused.

Wire volatile snapshots into the existing bounded transaction layers:

- public direct/prepared statement checkpoints roll back volatile rows on
  failed row-DML statements inside an active transaction;
- public direct/prepared transaction checkpoints open a transaction-level
  volatile snapshot and finish it on `COMMIT` or `ROLLBACK`;
- public direct/prepared savepoint frames open nested volatile snapshots and
  finish them on `ROLLBACK TO` or `RELEASE`;
- raw embedded MariaDB handler transaction and savepoint hooks open matching
  volatile snapshots around the handler-owned checkpoint state.

## Supported Scope

- Representative direct MEMORY/HEAP transaction rollback and savepoint
  rollback/release through `libmylite`.
- Representative prepared savepoint rollback/release through `libmylite`.
- Representative raw embedded MariaDB handler savepoint rollback/release for a
  routed MEMORY table.
- Full transaction rollback for runtime volatile MEMORY/HEAP rows.
- MEMORY/HEAP autoincrement gap preservation across savepoint and full
  transaction rollback.
- User temporary table rows, aliases, and create/drop lifecycle remain outside
  volatile transaction snapshots.

## Non-Goals

- Native MEMORY hash-index defaults.
- MEMORY memory accounting, maximum-size enforcement, or eviction behavior.
- MEMORY BLOB/TEXT column support.
- Removing `HA_NO_TRANSACTIONS` or claiming fully transactional MyLite engine
  flags.
- Crash recovery for process-local MEMORY/HEAP row contents; runtime-volatile
  rows remain empty after embedded runtime shutdown.
- Exhaustive native MEMORY/HEAP transaction parity.

## Compatibility Impact

`ENGINE=MEMORY` and `ENGINE=HEAP` routed tables now follow MyLite's bounded
direct/prepared transaction and savepoint rollback model for row visibility
while retaining runtime-only row lifetime. This moves MEMORY/HEAP
transaction/savepoint coverage from planned to partial.

Temporary tables remain deliberately non-transactional at the volatile-row
snapshot layer to preserve the existing MariaDB-compatible behavior already
covered by `test_row_dml_transactions`.

## File-Lifecycle Impact

No new durable file, journal, or sidecar is introduced. Snapshots are in-memory
copies of runtime volatile row state and are freed on commit, rollback, handle
close, or connection cleanup.

## Test And Verification Plan

- Extend storage-engine smoke coverage for MEMORY/HEAP direct transactions,
  direct savepoints, prepared savepoints, full rollback, autoincrement gap
  preservation, empty reopen behavior, and absence of durable MEMORY/HEAP row
  pages.
- Extend raw embedded handler savepoint smoke coverage for MEMORY savepoint
  rollback, release, full rollback, and autoincrement gap preservation.
- Keep existing temporary-table transaction tests passing to prove temporary
  volatile rows remain outside snapshots.
- Run the focused storage-engine and handler savepoint tests plus transaction
  compatibility groups.

## Acceptance Criteria

- Volatile snapshots restore routed MEMORY/HEAP row and supported index state
  at statement, transaction, and savepoint rollback boundaries.
- Commit and release paths keep committed volatile row changes.
- Rollback preserves generated MEMORY/HEAP autoincrement gaps.
- User temporary table rows and temporary table lifecycle are not rolled back by
  ordinary transaction rollback.
- No durable MyLite row pages or MariaDB MEMORY/HEAP table files are created.
