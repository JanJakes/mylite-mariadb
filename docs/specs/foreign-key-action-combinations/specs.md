# Foreign-Key Action Combinations

## Goal

Allow MyLite to publish and execute bounded foreign-key action combinations
whose individual action paths are already implemented: `ON DELETE CASCADE`,
`ON UPDATE CASCADE`, `ON DELETE SET NULL`, and `ON UPDATE SET NULL`. The DDL
gate should reject only combinations that include unsupported actions or child
table shapes, not valid pairs that the handler can already dispatch by
statement type.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/include/dict0mem.h` stores foreign-key action flags
  as independent bits: `DELETE_CASCADE`, `DELETE_SET_NULL`,
  `UPDATE_CASCADE`, and `UPDATE_SET_NULL`.
- `mariadb/storage/innobase/handler/ha_innodb.cc` and
  `mariadb/storage/innobase/handler/handler0alter.cc` OR delete and update
  action flags into the same foreign-key metadata object.
- `mariadb/storage/innobase/row/row0ins.cc:row_ins_foreign_check_on_constraint()`
  chooses the delete action for parent deletes and the update action for
  parent updates.
- `mariadb/storage/mylite/ha_mylite.cc` now mirrors that statement-type
  dispatch: parent deletes use `delete_action`; parent updates use
  `update_action`.

Official MariaDB documentation describes delete and update reference actions as
separate clauses on the same foreign key:

- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>

## Scope

Accept and execute these combinations when all referenced tables are durable
MyLite-routed base tables in the same primary `.mylite` file and the child
table shape satisfies every selected action:

- `ON DELETE CASCADE ON UPDATE CASCADE`;
- `ON DELETE CASCADE ON UPDATE SET NULL`;
- `ON DELETE SET NULL ON UPDATE CASCADE`;
- existing restrict-like pairings with any one supported action;
- existing `ON DELETE SET NULL ON UPDATE SET NULL`.

## Non-Goals

- `SET DEFAULT`.
- Recursive update-cascade graphs beyond the direct bounded update-cascade
  subset.
- Generated-column, BLOB/TEXT, partitioned, temporary, volatile, BLACKHOLE, or
  cross-file action-combination support.
- Full InnoDB lock ordering, deadlock detection, or cyclic action parity.

## Design

Replace the current mutually exclusive DDL action gate with independent action
validation:

1. classify the requested update action and delete action separately;
2. reject unsupported actions immediately;
3. require the bounded update-cascade table shape when update action is
   `CASCADE`;
4. require nullable child FK columns when either action is `SET NULL`;
5. require the bounded delete-cascade table shape when delete action is
   `CASCADE`;
6. accept the FK only when both requested actions pass.

No handler dispatch change should be necessary. Parent deletes already choose
`delete_action`; parent updates already choose `update_action`.

## Compatibility Impact

MyLite can claim supported action combinations over the existing bounded
action subsets. Broader recursive update-cascade graphs, generated/BLOB child
tables, `SET DEFAULT`, partitions, and cyclic action parity remain unsupported.

## Single-File And Lifecycle Impact

No file-format or companion-file change is introduced. Combined actions use the
same row, index, and statement-checkpoint paths as their individual actions.

## Public API, Build, Size, License

No public API, dependency, license, or intentional size-profile change is
introduced.

## Test And Verification Plan

- Verify `ON DELETE CASCADE ON UPDATE CASCADE` publishes, survives
  close/reopen, cascades parent-key updates, and cascades parent deletes.
- Verify `ON DELETE SET NULL ON UPDATE CASCADE` publishes, cascades updates,
  and sets child columns `NULL` on parent delete.
- Verify `ON DELETE CASCADE ON UPDATE SET NULL` publishes, sets child columns
  `NULL` on parent update, and cascades deletes.
- Keep rejecting `SET DEFAULT` and unsupported child row shapes before catalog
  publication.
- Run `git diff --check`, the storage-smoke archive build, the focused
  storage-engine smoke binary, `ctest --preset storage-smoke-dev`, and
  `ctest --preset dev`.

## Acceptance Criteria

- Supported action combinations are accepted only when every selected action's
  existing table-shape requirements pass.
- Update and delete statements dispatch the correct action from the same
  stored FK metadata.
- Rejected combinations leave no catalog metadata behind.
- Docs and compatibility matrices distinguish supported action combinations
  from recursive action graphs and unsupported `SET DEFAULT`.

## Risks And Open Questions

- Combined actions increase the chance of application-level cycles; recursive
  update-cascade graph handling remains a separate slice.
- The direct update-cascade limitation still applies when update action is
  `CASCADE`, even when the delete action is fully cascaded.
