# Foreign-Key Multi-Table Action Matrix

## Goal

Cover representative multi-table `UPDATE` and `DELETE` statements that target
the parent table and dispatch MyLite's existing bounded foreign-key actions.
The previous multi-table ordering slice covered `RESTRICT` / `NO ACTION`
statements where both parent and child were targets; this slice covers
parent-target join statements for `CASCADE` and `SET NULL`.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_update.cc:multi_update::initialize_tables()` and
  `multi_update::send_data()` collect matching joined rows and apply updates
  to target tables.
- `mariadb/sql/sql_update.cc:multi_update::do_updates()` replays delayed
  table updates after the join scan.
- `mariadb/sql/sql_delete.cc:multi_delete::initialize_tables()` and
  `multi_delete::send_data()` select target rows for multi-table deletes.
- `mariadb/sql/sql_delete.cc:multi_delete::do_deletes()` replays delayed
  table deletes after the join scan.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::update_row()` dispatches
  parent `ON UPDATE CASCADE` and `ON UPDATE SET NULL` actions before
  publishing the parent update.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::delete_row()` dispatches
  parent `ON DELETE CASCADE` and `ON DELETE SET NULL` actions before deleting
  the parent row.

Official MariaDB documentation describes update and delete reference actions
as storage-engine foreign-key behavior:

- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>
- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>

## Scope

Cover these representative shapes over durable MyLite-routed base tables:

- `DELETE parent FROM parent JOIN child ...` with `ON DELETE CASCADE`;
- `DELETE parent FROM parent JOIN child ...` with `ON DELETE SET NULL`;
- `UPDATE parent JOIN child ... SET parent.pk = ...` with
  `ON UPDATE CASCADE`;
- `UPDATE parent JOIN child ... SET parent.pk = ...` with
  `ON UPDATE SET NULL`.

## Non-Goals

- Exhaustive optimizer-selected join-order matrices.
- Multi-table statements that target both parent and child under action
  clauses.
- Cyclic or full recursive graph parity beyond the existing bounded acyclic
  update-cascade support.
- `SET DEFAULT`, generated-column, BLOB/TEXT, partitioned, temporary,
  volatile, BLACKHOLE, or cross-file action support.
- New lock ordering, deadlock detection, or isolation semantics.

## Compatibility Impact

MyLite can claim representative multi-table FK action coverage for parent-only
targets in joined update/delete statements. Broader exhaustive multi-table
matrices remain planned, especially statements that target both parent and
child while action clauses also mutate the child.

## Design

No storage-engine code change is expected. The multi-table SQL layer still
calls the same handler `update_row()` and `delete_row()` paths used by
single-table statements. Add smoke coverage that:

1. uses `STRAIGHT_JOIN` so the tested join shape is deterministic;
2. targets only the parent table, leaving child mutation to the FK action;
3. verifies child row deletion, `NULL` assignment, cascaded key copy, and
   forced-index visibility;
4. checks close/reopen persistence for every accepted result.

## File Lifecycle

No file-format or companion-file change is introduced. The statements mutate
ordinary row, row-state, index-entry, and FK metadata-visible state inside the
primary `.mylite` file using existing statement checkpoints.

## Embedded Lifecycle And API

No public API change is introduced. Direct SQL execution is sufficient because
this slice exercises storage-engine row mutation behind MariaDB multi-table
DML.

## Build, Size, And Dependencies

No dependency, license, or intended size-profile change is introduced.

## Test Plan

- Add storage-engine smoke coverage for multi-table parent-target
  `ON DELETE CASCADE`.
- Add multi-table parent-target `ON DELETE SET NULL`.
- Add multi-table parent-target `ON UPDATE CASCADE`.
- Add multi-table parent-target `ON UPDATE SET NULL`.
- Verify affected rows through normal scans and forced-index lookups.
- Verify accepted results after close/reopen.
- Run `git diff --check`, the focused storage-engine smoke binary,
  `ctest --preset storage-smoke-dev`, and `ctest --preset dev`.

## Acceptance Criteria

- Parent-target multi-table delete dispatches delete actions.
- Parent-target multi-table update dispatches update actions.
- Child row and index state match the action after close/reopen.
- Docs and compatibility matrices distinguish this representative coverage
  from exhaustive multi-table action matrices.

## Risks And Open Questions

Statements that target both parent and child under action clauses may expose
duplicate row-id, action ordering, or already-mutated-row behavior. They remain
future work until a separate slice specifies the exact MariaDB compatibility
target.
