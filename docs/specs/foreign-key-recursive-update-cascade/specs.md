# Foreign-Key Recursive UPDATE CASCADE

## Goal

Extend MyLite's bounded `ON UPDATE CASCADE` support from direct child rows to
acyclic recursive action graphs. When a cascaded child update changes a key that
is itself referenced by another table, MyLite should dispatch that child row's
parent actions before publishing the child update.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/row/row0ins.cc:row_ins_foreign_check_on_constraint()`
  constructs `UPDATE_CASCADE` and `UPDATE_SET_NULL` vectors for matching child
  records and then calls `row_update_cascade_for_mysql()`.
- `mariadb/storage/innobase/row/row0mysql.cc:row_update_cascade_for_mysql()`
  executes cascaded child updates through InnoDB's row-update machinery, so a
  child row that is also a parent can trigger its own FK actions.
- `mariadb/storage/innobase/include/dict0mem.h` caps cascade depth with
  `FK_MAX_CASCADE_DEL`, which is `15` on this base line.
- MyLite already has `MYLITE_FOREIGN_KEY_MAX_CASCADE_DEPTH`, parent-action
  dispatch, delete-cascade recursion, direct update-cascade row rewriting,
  duplicate-key checks, child FK checks, parent FK checks, and statement
  rollback checkpoints.

Official MariaDB documentation describes `CASCADE` as a storage-engine FK
action:

- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>

## Scope

Implement this bounded SQL subset:

- durable MyLite-routed base tables in the same primary `.mylite` file;
- recursive `ON UPDATE CASCADE` and `ON UPDATE SET NULL` actions reached from
  an update cascade;
- simple parent and child table row shapes without BLOB/TEXT or generated
  columns;
- acyclic action graphs within `MYLITE_FOREIGN_KEY_MAX_CASCADE_DEPTH`;
- statement rollback when a recursive action is blocked by a duplicate key,
  retained FK check, or unsupported action shape.

## Non-Goals

- Cyclic update-cascade parity with InnoDB.
- Deferrable checks or full set-wide action graph planning.
- `SET DEFAULT`.
- Generated-column, BLOB/TEXT, partitioned, temporary, volatile, BLACKHOLE, or
  cross-file action support.
- Full InnoDB lock ordering, deadlock detection, or isolation semantics.

## Design

During `mylite_apply_update_cascade_to_child_rows()`, MyLite already builds the
candidate child row and performs duplicate-key and child-FK validation before
writing it. Insert recursive parent-action dispatch in the same place
`ha_mylite::update_row()` uses it:

1. copy the matching child row to `record[1]` and the candidate new row to
   `record[0]`;
2. apply same-row update actions for the child table before index entries are
   prepared;
3. prepare index entries and check duplicate keys;
4. check child FKs, skipping only the in-flight cascaded constraint whose
   parent row is not published yet;
5. call `mylite_apply_parent_foreign_key_actions()` with the old and new child
   rows and `cascade_depth + 1`;
6. run `mylite_check_parent_foreign_keys()` for restrictive child-as-parent
   relationships;
7. publish the child row and index entries together.

The existing statement checkpoint owns atomicity. If any recursive action
fails, earlier cascaded child updates and the original parent update roll back.

## Compatibility Impact

MyLite can claim bounded recursive update actions for simple acyclic FK graphs.
Broader exhaustive multi-table matrices, cyclic parity, `SET DEFAULT`, and
transaction-aware FK checks remain planned.

## Single-File And Lifecycle Impact

No file-format or companion-file change is introduced. Recursive cascades are
ordinary row and index updates inside the primary `.mylite` file and use the
existing statement checkpoint for rollback.

## Public API, Build, Size, License

No public API, dependency, license, or intentional size-profile change is
introduced.

## Test And Verification Plan

- Verify a root primary-key update cascades through a child table whose primary
  key is also the FK, then updates a grandchild `ON UPDATE CASCADE` FK.
- Verify the same recursive path can dispatch a grandchild `ON UPDATE SET NULL`
  action.
- Verify a restrictive grandchild blocks the recursive update and the statement
  rolls back parent and child rows.
- Verify recursive results survive close/reopen and forced-index reads.
- Run `git diff --check`, the storage-smoke archive build, the focused
  storage-engine smoke binary, `ctest --preset storage-smoke-dev`, and
  `ctest --preset dev`.

## Acceptance Criteria

- Recursive update actions execute before publishing the cascaded child row.
- Recursive action failures roll back the full statement.
- Docs and compatibility matrices describe bounded recursive update actions
  without claiming cyclic or full InnoDB graph parity.

## Risks And Open Questions

- The implementation remains scan-based and depth-capped. A future general
  graph executor may need ancestor tracking for closer InnoDB cyclic-update
  behavior and better performance on larger data sets.
