# Foreign-Key DELETE CASCADE Actions

## Goal

Add the first bounded `ON DELETE CASCADE` action path for MyLite-routed
foreign keys. Deleting a parent row should delete matching child rows before
the parent row is removed, while preserving statement rollback and rejecting
unsupported action graphs before they can leave orphaned rows.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/row/row0ins.cc:row_ins_foreign_check_on_constraint()`
  handles parent-row deletes by requiring `DELETE_CASCADE` or
  `DELETE_SET_NULL`; otherwise it reports the row as referenced.
- The same InnoDB function sets `cascade->is_delete = PLAIN_DELETE` for
  `DELETE_CASCADE` and routes the child delete through
  `row_update_cascade_for_mysql()`.
- `mariadb/storage/innobase/row/row0mysql.cc:row_update_cascade_for_mysql()`
  increments `fk_cascade_depth` and fails when the depth exceeds
  `FK_MAX_CASCADE_DEL`.
- `mariadb/storage/innobase/include/dict0mem.h` defines
  `FK_MAX_CASCADE_DEL` as `15` and stores FK action flags including
  `DELETE_CASCADE`, `DELETE_SET_NULL`, `UPDATE_CASCADE`, and
  `UPDATE_SET_NULL`.
- `mariadb/storage/mylite/ha_mylite.cc` already has the required MyLite
  building blocks: parent-FK metadata listing, transient catalog-backed child
  `TABLE` opening for non-self actions, live-row scans, row-id-aware index
  prefix checks, parent action dispatch for `SET NULL`, and statement
  checkpoints for rollback.

Official MariaDB documentation describes `CASCADE` as a storage-engine FK
action:

- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>

## Scope

Implement this public SQL subset:

- durable MyLite-routed base tables in the same primary `.mylite` file;
- self-referencing and non-self foreign keys;
- `ON DELETE CASCADE`;
- `ON UPDATE` omitted, `RESTRICT`, or `NO ACTION`;
- supported ordinary child key prefixes;
- simple child table row shapes without BLOB/TEXT or generated columns;
- recursive deletes through already-supported `SET NULL` and new
  `DELETE CASCADE` action paths, capped at 15 levels.

## Non-Goals

- `ON UPDATE CASCADE`; child-key value rewrites need a separate slice.
- `SET DEFAULT`; the selected MariaDB/InnoDB base still does not implement it.
- Generated-column, BLOB/TEXT, partitioned, temporary, volatile, BLACKHOLE, or
  cross-file cascade support.
- Full InnoDB lock ordering, deadlock detection, cyclic cascade parity, or
  deferrable action graphs. Cycles may still fail through the depth cap.
- Broad multi-table action ordering matrices beyond the representative direct
  and nested cases in this slice.

## Design

DDL validation should accept `ON DELETE CASCADE` when the update action remains
restrict-like and the child table shape is in the bounded action subset. Other
new action combinations remain rejected before catalog publication.

During `ha_mylite::delete_row()`, MyLite already lists parent FK metadata before
checking whether the parent row may be removed. For `DELETE CASCADE` metadata,
the handler should:

1. resolve the parent key on the parent `TABLE` and the child key on either the
   same `TABLE` or a transient catalog-backed child `TABLE`;
2. encode the old parent key into the child-key nullable format;
3. scan live child rows and skip the parent row currently being deleted for
   self-referencing constraints;
4. find child rows whose FK key prefix matches the old parent key;
5. materialize the child row into the child `TABLE` record buffer;
6. recursively apply parent FK actions for that child delete, incrementing a
   MyLite cascade depth counter and failing once it exceeds 15;
7. run the ordinary parent-FK checks for the child delete so unrelated
   `RESTRICT` / `NO ACTION` constraints still block the operation;
8. delete the child row through `mylite_storage_delete_row()`;
9. let the original parent delete continue once all matching child rows are
   handled.

The existing statement checkpoint owns atomicity. If a recursive action or
later parent check fails, every child delete and `SET NULL` side effect from
the statement must roll back.

## Compatibility Impact

MyLite can claim bounded `ON DELETE CASCADE` support for simple routed tables.
Foreign-key actions remain partial: `ON UPDATE CASCADE`, `SET DEFAULT`,
generated/BLOB child tables, partitioned tables, cross-file cascades, cyclic
parity, and broader action-order matrices remain unsupported.

## Single-File And Lifecycle Impact

No file-format or companion-file change is introduced. Cascaded child deletes
are ordinary row-state updates inside the primary `.mylite` file and are
covered by the current statement checkpoint and recovery model.

## Public API, Build, Size, License

No public API, dependency, license, or intentional size-profile change is
introduced.

## Test And Verification Plan

- Accept self-referencing and non-self `ON DELETE CASCADE` DDL and expose the
  action through `SHOW CREATE TABLE` before and after close/reopen.
- Verify parent deletes remove multiple direct child rows, preserve unaffected
  child rows, and remove child index entries.
- Verify nested cascades through a second child table.
- Verify statement rollback when a cascaded child delete is blocked by another
  `RESTRICT` / `NO ACTION` child table.
- Keep rejecting `ON UPDATE CASCADE`, `SET DEFAULT`, and unsupported child row
  shapes before catalog publication.
- Run `git diff --check`, the storage-smoke archive build, the focused
  storage-engine smoke binary, `ctest --preset storage-smoke-dev`, and
  `ctest --preset dev`.

## Acceptance Criteria

- Supported self and non-self parent deletes cascade child row deletes without
  orphaning rows or index entries.
- Nested supported cascades work up to the bounded depth cap.
- Unsupported or blocked cascades fail atomically with statement rollback.
- Docs and compatibility matrices describe the bounded delete-cascade subset
  without claiming update-cascade or full action-graph parity.

## Risks And Open Questions

- Recursive deletes over cyclic data may still fail through the depth cap
  rather than matching every InnoDB edge case.
- The implementation scans child rows rather than using targeted child-index
  traversal. That is acceptable for the current smoke scope but should be
  revisited with larger compatibility workloads.
- Future write-concurrency work must revisit lock ordering and deadlock
  behavior for action graphs.
