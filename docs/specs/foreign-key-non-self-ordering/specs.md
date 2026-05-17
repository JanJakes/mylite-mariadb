# Foreign-Key Non-Self Ordering

## Problem

MyLite has representative ordered FK coverage for self-referential statements,
but non-self parent statements still need deterministic coverage for the
supported `RESTRICT` / `NO ACTION` subset. The important risk is failed parent
updates or deletes after an earlier row in the same statement was already
published: statement rollback must restore the earlier row mutation.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_delete.cc` calls `TABLE::delete_row()` for each deleted row,
  which delegates to `handler::ha_delete_row()`.
- `mariadb/sql/sql_update.cc` scans candidate rows and calls
  `handler::ha_update_row()` for each row on non-batched paths.
- `mariadb/sql/handler.cc:handler::ha_update_row()` and
  `handler::ha_delete_row()` call the storage engine's row operation and
  surface handler errors to the statement executor.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::update_row()` checks child
  FKs for the new row and parent FKs for changed referenced keys before
  publishing the replacement row.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::delete_row()` checks parent
  FKs before publishing the delete marker.
- `mariadb/storage/mylite/ha_mylite.cc:mylite_check_parent_foreign_key()`
  skips unchanged referenced keys and otherwise checks whether a child key
  prefix exists in the current primary-file index state.

## Design

Add non-self parent ordering and rollback coverage without changing FK
semantics:

1. A deterministic parent `DELETE ... ORDER BY` first deletes an unreferenced
   parent row, then reaches a referenced parent row and fails.
2. The failed delete must roll back the earlier unreferenced-row delete and
   preserve the referenced parent/child rows.
3. A deterministic parent `UPDATE ... ORDER BY` first updates an unreferenced
   parent key, then reaches a referenced parent key and fails.
4. The failed update must roll back the earlier unreferenced-row update and
   preserve the referenced parent/child rows.
5. Do not implement multi-table parent/child statement ordering, cascades,
   `SET NULL`, `SET DEFAULT`, or deferred graph validation in this slice.

## Compatibility Impact

This is partial MySQL/MariaDB-compatible behavior for MyLite's current FK
subset: checks are immediate and statement rollback preserves the pre-statement
state when a later row fails. It does not claim deferrable constraints or
optimizer-independent ordering for unordered statements.

## Affected Subsystems

- `packages/libmylite/tests/embedded_storage_engine_test.c`: SQL regression
  coverage for non-self parent update/delete ordering and rollback.
- Documentation and compatibility matrices: move representative non-self
  parent update/delete rollback coverage from planned to covered partial
  behavior.
- `mariadb/storage/mylite/ha_mylite.cc`: code changes only if the tests reveal
  a mismatch.

## Single-File And Embedded Lifecycle

No file-format change is intended. The slice uses existing row pages, index
entries, FK metadata, and statement rollback checkpoints in the primary
`.mylite` file. No durable MariaDB sidecar or new MyLite companion file is
introduced.

## Public API, Build, Size, And Dependencies

No public `libmylite` C API, build-profile, size-profile, license, or
dependency change is intended.

## Test Plan

- Create a non-self parent table and child FK table.
- Verify ordered parent delete fails after an earlier unreferenced parent row
  would have been deleted.
- Verify the failed delete preserves both parent rows and the child row.
- Verify ordered parent update fails after an earlier unreferenced parent row
  would have been updated.
- Verify the failed update preserves both parent rows and the child row.
- Verify the accepted state remains visible after close/reopen.
- Run focused storage-engine build/test targets, `ctest --preset
  storage-smoke-dev`, `ctest --preset dev`, and `git diff --check`.

## Acceptance Criteria

- Non-self parent update/delete ordering has SQL-level coverage.
- Failed parent statements roll back earlier same-statement row mutations.
- Parent and child state remains correct after close/reopen.
- Docs distinguish this representative coverage from broader multi-table,
  cascade, `SET NULL`, `SET DEFAULT`, and deferred validation work.

## Implementation Status

Implemented in this slice:

- SQL storage-engine coverage rejects an ordered non-self parent delete after
  the statement first visits an unreferenced parent row.
- SQL storage-engine coverage rejects an ordered non-self parent update after
  the statement first visits an unreferenced parent row.
- Reopen checks verify failed statements preserved the original parent and
  child state.

## Risks And Open Questions

- The tests use deterministic single-table `ORDER BY`. They do not claim
  ordering for unordered statements.
- Multi-table parent/child update/delete statements still need a separate slice
  because their execution order can depend on MariaDB's multi-table plan and
  target-table update order.
