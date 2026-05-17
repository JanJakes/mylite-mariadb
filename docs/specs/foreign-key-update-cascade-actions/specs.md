# Foreign-Key UPDATE CASCADE Actions

## Goal

Add the first bounded `ON UPDATE CASCADE` action path for MyLite-routed
foreign keys. Updating a referenced parent key should rewrite matching child
foreign-key columns to the new parent key before the statement commits, without
leaving orphaned rows, stale index entries, or unsupported action combinations
in the MyLite catalog.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/row/row0ins.cc:row_ins_foreign_check_on_constraint()`
  handles parent-key updates by requiring `UPDATE_CASCADE` or
  `UPDATE_SET_NULL`; otherwise it reports the parent row as referenced.
- The same function builds a cascade update vector for `UPDATE_CASCADE`, and
  fails when the cascaded child value would not fit or would assign `NULL` to a
  non-null child column.
- InnoDB rejects cyclic cascaded updates of the same table in
  `row_ins_foreign_check_on_constraint()` to avoid infinite update loops and
  inconsistent in-progress parent indexes.
- `mariadb/storage/innobase/row/row0mysql.cc:row_update_cascade_for_mysql()`
  increments `fk_cascade_depth` and fails when depth exceeds
  `FK_MAX_CASCADE_DEL`.
- `mariadb/storage/innobase/include/dict0mem.h` defines
  `FK_MAX_CASCADE_DEL` as `15` and records `UPDATE_CASCADE` separately from
  `DELETE_CASCADE`, `DELETE_SET_NULL`, and `UPDATE_SET_NULL`.
- `mariadb/storage/mylite/ha_mylite.cc` already has parent FK action
  dispatch, same-row update action handling for `UPDATE SET NULL`, child table
  scans, duplicate-key checks, row-id-preserving index replacement, and
  statement checkpoints for rollback.

Official MariaDB documentation describes `CASCADE` as a storage-engine FK
action:

- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>

## Scope

Implement this public SQL subset:

- durable MyLite-routed base tables in the same primary `.mylite` file;
- self-referencing and non-self foreign keys;
- `ON UPDATE CASCADE`;
- `ON DELETE` omitted, `RESTRICT`, or `NO ACTION`;
- supported ordinary child key prefixes and exact referenced parent keys;
- simple parent and child table row shapes without BLOB/TEXT or generated
  columns;
- direct parent-to-child cascades, including same-row self-references, with
  statement rollback on duplicate-key or FK-check failure.

## Non-Goals

- `ON UPDATE CASCADE` combined with `ON DELETE CASCADE` or `ON DELETE SET NULL`;
  action combinations can be broadened after the direct update path is stable.
- Recursive update-cascade action graphs beyond the direct matching child rows
  updated by the triggering parent row.
- Cyclic update-cascade parity with InnoDB.
- `SET DEFAULT`; the selected MariaDB/InnoDB base still does not implement it.
- Generated-column, BLOB/TEXT, partitioned, temporary, volatile, BLACKHOLE, or
  cross-file update-cascade support.
- Full InnoDB lock ordering, deadlock detection, or deferrable action graphs.

## Design

DDL validation should accept `ON UPDATE CASCADE` only when the delete action is
restrict-like and the child table shape is in the bounded direct action subset.
Other action combinations remain rejected before catalog publication.

During `ha_mylite::update_row()`, MyLite already handles same-row
self-referencing `UPDATE SET NULL` before building new index entries. Extend
that same-row phase so an updated row that was both parent and child of itself
rewrites its own child columns to the new parent key when they still point at
the old parent key. This keeps duplicate checks, child checks, and the final
row payload aligned with the cascaded value.

For non-current matching child rows, the parent action dispatcher should:

1. resolve the parent key on the parent `TABLE` and the child key on either the
   same `TABLE` or a transient catalog-backed child `TABLE`;
2. encode the old parent key into the child-key nullable format and return
   early if the old parent key contains `NULL`;
3. encode the new parent key and return early when the parent key prefix did
   not change;
4. scan live child rows and skip the parent row currently being updated for
   self-referencing constraints;
5. find child rows whose FK key prefix matches the old parent key;
6. materialize the old child row into `record[1]`, copy it to `record[0]`, and
   copy each referenced parent-key column from the parent new row into the
   corresponding child FK column;
7. reject the child update if a parent `NULL` would be written into a non-null
   child column;
8. rebuild child index entries, run duplicate-key checks, and run child FK
   checks that are not the in-flight cascaded constraint;
9. update the child row through `mylite_storage_update_row_with_index_entries()`
   so row payload and index entries move together.

The existing statement checkpoint owns atomicity. If any child rewrite fails,
all child changes and the original parent update roll back.

## Compatibility Impact

MyLite can claim bounded direct `ON UPDATE CASCADE` support for simple routed
tables. Foreign-key actions remain partial: action combinations, recursive
update-cascade graphs, generated/BLOB child tables, partitioned tables,
cross-file cascades, cyclic parity, and broader action-order matrices remain
unsupported.

## Single-File And Lifecycle Impact

No file-format or companion-file change is introduced. Cascaded child updates
are ordinary row and index updates inside the primary `.mylite` file and are
covered by the current statement checkpoint and recovery model.

## Public API, Build, Size, License

No public API, dependency, license, or intentional size-profile change is
introduced.

## Test And Verification Plan

- Accept self-referencing and non-self `ON UPDATE CASCADE` DDL with
  restrict-like delete action and expose the action through `SHOW CREATE TABLE`
  before and after close/reopen.
- Verify parent-key updates rewrite multiple direct child rows, preserve
  unaffected child rows, and move child index entries from the old parent key
  to the new parent key.
- Verify same-row self-reference updates rewrite the row's own child FK column
  when it still points at the old parent key.
- Verify statement rollback when a cascaded child update would violate a
  duplicate key or another retained FK check.
- Keep rejecting unsupported action combinations, `SET DEFAULT`, and
  unsupported child row shapes before catalog publication.
- Run `git diff --check`, the storage-smoke archive build, the focused
  storage-engine smoke binary, `ctest --preset storage-smoke-dev`, and
  `ctest --preset dev`.

## Acceptance Criteria

- Supported self and non-self parent-key updates cascade direct child FK column
  rewrites without orphaning rows or index entries.
- Same-row self-referencing updates persist the cascaded child value in the
  updated parent row.
- Unsupported or blocked cascades fail atomically with statement rollback.
- Docs and compatibility matrices describe the direct bounded update-cascade
  subset without claiming recursive action-graph parity.

## Risks And Open Questions

- Direct child FK checks need to account for the parent row's new key before
  the parent update has been published to storage.
- Recursive update-cascade graphs need a later ancestor-tracking design rather
  than relying only on a depth cap.
- The implementation scans child rows rather than using targeted child-index
  traversal. That is acceptable for the current smoke scope but should be
  revisited with larger compatibility workloads.
