# Foreign-Key Multi-Table Ordering

## Problem

MyLite has single-table FK ordering coverage for self-referential and non-self
parent statements, but multi-table `UPDATE` / `DELETE` statements can touch a
child and parent table in one statement. The remaining ordering risk is whether
MyLite's immediate FK checks observe the same row-operation order MariaDB uses
for the target tables, and whether failed parent-first statements roll back
correctly.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_update.cc:multi_update::initialize_tables()` records row ids
  for updated target tables, while `safe_update_on_fly()` can let the first
  table in the join be updated during row scanning.
- `mariadb/sql/sql_update.cc:multi_update::send_data()` applies the
  `table_to_update` row immediately and stores row ids plus new values for
  other updated tables.
- `mariadb/sql/sql_update.cc:multi_update::do_updates()` replays delayed
  target-table updates after the scan in `update_tables` order.
- `mariadb/sql/sql_delete.cc:multi_delete::initialize_tables()` decides whether
  the first target table can be deleted while scanning and creates row-id
  temporary tables for delayed targets.
- `mariadb/sql/sql_delete.cc:multi_delete::send_data()` deletes the current
  `table_being_deleted` while scanning when allowed and stores row ids for
  delayed delete targets.
- `mariadb/sql/sql_delete.cc:multi_delete::do_deletes()` replays delayed
  target-table deletes after the scan.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::update_row()` and
  `delete_row()` perform immediate FK checks before each storage mutation, so
  target-table order matters for `RESTRICT` / `NO ACTION`.

## Design

Add representative multi-table FK ordering coverage for the current
`RESTRICT` / `NO ACTION` subset:

1. A parent-first multi-table delete fails because the parent row is checked
   before the matching child row is removed.
2. A child-first multi-table delete succeeds because the child row is removed
   before the parent delete is replayed.
3. A parent-first multi-table update fails because the parent key is checked
   before the matching child reference is cleared.
4. A child-first multi-table update succeeds because the child reference is
   cleared before the parent key update is replayed.
5. Use `STRAIGHT_JOIN` to make the tested target-table order explicit.
6. Do not implement cascades, `SET NULL`, `SET DEFAULT`, or deferred graph
   validation in this slice.

## Compatibility Impact

This narrows MyLite's MySQL/MariaDB compatibility claim for the existing FK
subset: multi-table statements are immediate and order-sensitive, not
deferred. The coverage verifies representative accepted and rejected orders
without claiming exhaustive optimizer-independent behavior.

## Affected Subsystems

- `packages/libmylite/tests/embedded_storage_engine_test.c`: SQL regression
  coverage for representative multi-table update/delete ordering.
- Documentation and compatibility matrices: record representative multi-table
  FK ordering as covered and keep broader FK actions planned.
- `mariadb/storage/mylite/ha_mylite.cc`: code changes only if the tests reveal
  a mismatch.

## Single-File And Embedded Lifecycle

No file-format change is intended. The behavior uses existing row pages, index
entries, FK metadata, and statement rollback checkpoints in the primary
`.mylite` file. No durable MariaDB sidecar or new MyLite companion file is
introduced.

## Public API, Build, Size, And Dependencies

No public `libmylite` C API, build-profile, size-profile, license, or
dependency change is intended.

## Test Plan

- Create a parent table and a child table with a supported FK.
- Verify parent-first multi-table delete fails and preserves both rows.
- Verify child-first multi-table delete succeeds and removes both rows.
- Verify parent-first multi-table update fails and preserves both rows.
- Verify child-first multi-table update succeeds, clearing the child reference
  and changing the parent key.
- Verify accepted and rejected states after close/reopen.
- Run focused storage-engine build/test targets, `ctest --preset
  storage-smoke-dev`, `ctest --preset dev`, and `git diff --check`.

## Acceptance Criteria

- Representative multi-table parent-first failure and child-first success have
  SQL-level coverage for update and delete.
- Failed parent-first statements preserve their pre-statement state.
- Accepted child-first statements remain visible after close/reopen.
- Docs distinguish representative ordering coverage from FK actions and
  deferrable validation.

## Implementation Status

Implemented in this slice:

- SQL storage-engine coverage rejects parent-first multi-table delete and
  update statements.
- SQL storage-engine coverage accepts child-first multi-table delete and update
  statements.
- Reopen checks verify the resulting parent and child state.

## Risks And Open Questions

- The tests force join order with `STRAIGHT_JOIN`; broader optimizer-selected
  orders and larger target-table matrices remain future compatibility work.
- FK actions still need a separate mutation design because cascades and
  `SET NULL` require storage-engine-owned child-row mutation rather than simple
  parent existence checks.
