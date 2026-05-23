# MTR routed storage volatile transaction smoke

## Problem

The storage-routed MTR list proves durable routed transaction behavior and
simple MEMORY/HEAP row visibility, while first-party tests prove volatile
MEMORY/HEAP statement, transaction, and savepoint snapshots. The raw embedded
MTR storage list should also exercise representative volatile snapshot behavior
so the MariaDB handler transaction path stays covered outside `libmylite` test
wrappers.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::trans_register_ha()` registers storage engines for
  statement and normal transaction boundaries.
- `mariadb/sql/transaction.cc::trans_commit_stmt()` and
  `trans_rollback_stmt()` drive statement commit/rollback through handler
  callbacks.
- `mariadb/storage/mylite/ha_mylite.cc` wires MyLite handlerton `commit`,
  `rollback`, `savepoint_set`, `savepoint_rollback`, and `savepoint_release`
  callbacks, with volatile statement, transaction, and savepoint snapshots.
- `mariadb/storage/mylite/mylite_volatile_rows.cc` copies only snapshot-
  participating routed MEMORY/HEAP tables for rollback, leaving user temporary
  tables outside those snapshots.

## Design

Add `mylite.routed_storage_volatile_transactions` to the storage MTR list. The
test runs with a primary `.mylite` file and enforced MyLite storage, creates
one routed `ENGINE=MEMORY` table and one routed `ENGINE=HEAP` table, and then
checks representative raw MariaDB behavior:

- full transaction rollback removes MEMORY and HEAP rows;
- savepoint rollback restores MEMORY and HEAP row visibility while preserving
  MEMORY autoincrement gaps;
- savepoint release and commit keep rows written after rollback;
- failed duplicate statements over MEMORY and HEAP leave no partial rows; and
- the standard routed-storage sidecar assertion remains clean.

## Scope

This is test and documentation work only. It does not change volatile-row
snapshot implementation, native MEMORY hash-index defaults, memory accounting,
BLOB/TEXT restrictions, or crash-recovery semantics for volatile rows.

## Compatibility Impact

The storage MTR runner gains direct evidence that raw embedded MariaDB
transaction and statement callbacks protect routed MEMORY/HEAP volatile row
state. Compatibility remains partial because broader native MEMORY/HEAP parity
and crash recovery for volatile row contents remain out of scope.

## Storage And Lifecycle Impact

Routed MEMORY/HEAP rows stay process-local. The test must not publish durable
row or index pages for those tables, and it must not create native MEMORY/HEAP
sidecars.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No binary-size or dependency impact; this adds only MTR test and documentation
coverage.

## Verification Plan

- `tools/mylite-mtr-harness probe-storage
  mylite.routed_storage_volatile_transactions`
- `tools/mylite-mtr-harness run-storage
  mylite.routed_storage_volatile_transactions`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- MEMORY and HEAP rows roll back on full transaction rollback.
- MEMORY and HEAP row visibility rolls back to a savepoint.
- MEMORY autoincrement gaps survive savepoint and full transaction rollback.
- Failed duplicate statements leave no partial MEMORY or HEAP rows.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention raw storage-routed volatile
  transaction coverage.

## Risks

This remains representative raw-MTR coverage. It does not replace the richer
first-party MEMORY/HEAP matrix for close/reopen volatility, prepared
savepoints, temporary-table exclusion, overflow boundaries, or broader native
MEMORY semantics.
