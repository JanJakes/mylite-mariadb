# Foreign-Key Non-Self SET NULL Actions

## Goal

Extend MyLite's bounded foreign-key action support from same-table rows to
simple non-self `ON DELETE SET NULL` and `ON UPDATE SET NULL` constraints.
Deleting or updating a parent key in one MyLite-routed table should set matching
child foreign-key columns in another MyLite-routed table to SQL `NULL` before
the parent row change is published.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_class.h:Foreign_key` records `FK_OPTION_SET_NULL` for both
  update and delete actions. `mariadb/sql/table.cc` renders those actions from
  stored FK metadata for `SHOW CREATE TABLE`.
- `mariadb/storage/innobase/handler/handler0alter.cc:innobase_set_foreign_key_option()`
  maps `FK_OPTION_SET_NULL` to InnoDB update/delete action flags, and
  `innobase_check_fk_option()` rejects `SET NULL` when any child FK column is
  `NOT NULL`.
- `mariadb/storage/innobase/row/row0ins.cc:row_ins_foreign_check_on_constraint()`
  executes delete and update actions inside the engine row path. For
  `SET NULL`, it builds an update vector that sets the child key fields to SQL
  `NULL`, locks the child clustered record, and routes the change through
  `row_update_cascade_for_mysql()`.
- `mariadb/sql/table.h:open_table_from_share()` and
  `mariadb/sql/handler.cc:ha_create_table_from_share()` show the local pattern
  for opening a `TABLE` object from an already-initialized `TABLE_SHARE` and
  closing it with `closefrm()`.
- `mariadb/storage/mylite/ha_mylite.cc` already stores MyLite catalog FRM
  images and can initialize a catalog-backed `TABLE_SHARE` for non-self FK
  existence checks. The missing piece is opening that share as a real `TABLE`
  so child fields, record buffers, key definitions, row serialization, and
  index recomputation can be reused for child-row action updates.

Official MariaDB documentation describes `SET NULL` as a storage-engine FK
action and requires nullable child columns:

- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>
- <https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimization-and-indexes/foreign-keys>

## Scope

Implement this public SQL subset:

- durable MyLite-routed base parent and child tables in the same primary
  `.mylite` file;
- non-self foreign keys where the child table and parent table differ;
- `ON DELETE SET NULL`;
- `ON UPDATE SET NULL`;
- nullable child FK columns backed by a supported ordinary child key;
- simple child table row shapes without generated columns or BLOB/TEXT fields.

Self-referencing `SET NULL` actions remain covered by the existing self-action
slices.

## Non-Goals

- Recursive action chains. Rows changed by a non-self `SET NULL` action are
  checked for immediate FK validity, but they do not trigger further parent
  actions in this slice.
- `CASCADE`, `SET DEFAULT`, or mixed action graph ordering beyond immediate
  `RESTRICT` / `NO ACTION` checks plus this bounded `SET NULL` action.
- Same-row self-update action edge cases.
- Generated-column, BLOB/TEXT, partitioned, temporary, volatile, BLACKHOLE, or
  cross-file action support.
- Full InnoDB lock ordering, deadlock detection, or recursive cascade-depth
  compatibility. Those belong with future write-concurrency and action-graph
  work.

## Design

DDL validation should accept `SET NULL` on non-self constraints when the child
table shape already satisfies MyLite's bounded action rules: all child FK
columns are nullable, the child key exists, and the child row format is simple.
Unsupported action shapes must still fail before MyLite catalog publication.

During `ha_mylite::delete_row()` and `ha_mylite::update_row()`, MyLite already
lists parent FK metadata before checking whether the parent row may change. For
non-self `SET NULL` metadata, the handler should:

1. initialize a catalog-backed child `TABLE_SHARE` from the stored MyLite FRM
   image;
2. open a transient child `TABLE` from that share with `open_table_from_share()`
   and close it with `closefrm()`;
3. resolve the current parent key on the already-open parent `TABLE` and the
   child key on the transient child `TABLE`;
4. encode the old parent key into the child-key nullable format;
5. return early when the old parent key is `NULL`, and for updates when the
   parent key is unchanged;
6. scan live child rows from the MyLite storage file;
7. set matching child FK columns to SQL `NULL` in the child `TABLE` record
   buffer;
8. recompute supported index entries and the row payload;
9. rerun duplicate-key, child-FK, and parent-FK checks for the mutated child
   row;
10. publish the child-row update through
    `mylite_storage_update_row_with_index_entries()`.

The existing statement checkpoint owns atomicity. If a later `RESTRICT` /
`NO ACTION` check blocks the parent update/delete, the child `SET NULL` side
effects must roll back with the failed statement.

## Affected Subsystems

- MyLite storage-engine handler FK action code.
- MyLite catalog-backed FRM table opening helper.
- Storage-engine smoke tests for direct SQL DML, close/reopen metadata, indexed
  reads, and statement rollback.
- Storage architecture, compatibility matrix, and roadmap documentation.

## Compatibility Impact

MyLite can claim non-self `ON DELETE SET NULL` and `ON UPDATE SET NULL` over the
bounded table shapes above. Foreign-key actions remain partial: cascades,
`SET DEFAULT`, recursive action chains, generated/BLOB child tables, and broader
multi-table action matrices remain planned.

## Single-File And Lifecycle Impact

No file-format or companion-file change is introduced. Child action updates are
ordinary MyLite row and index-entry updates inside the primary `.mylite` file.
The transient child `TABLE` exists only for the handler call and must not create
durable MariaDB metadata sidecars.

## Public API, Build, Size, License

No public API, build profile, dependency, license, or intentional size-profile
change is introduced.

## Test And Verification Plan

- Accept non-self `ON DELETE SET NULL` and `ON UPDATE SET NULL` DDL and expose
  the actions through `SHOW CREATE TABLE` before and after close/reopen.
- Verify parent deletes set multiple child rows to `NULL`, remove the parent
  row, preserve child index reads, and leave old parent keys unusable.
- Verify parent updates set matching child rows to `NULL`, preserve indexed
  reads for unaffected children, and allow new children for the updated parent
  key.
- Verify action side effects roll back when another FK on the same child table
  blocks the parent delete or update.
- Keep rejecting `SET NULL` on `NOT NULL` child columns and unsupported child
  row shapes before catalog publication.
- Run `git diff --check`, focused storage-engine smoke coverage,
  `ctest --preset storage-smoke-dev`, and `ctest --preset dev`.

## Acceptance Criteria

- Supported non-self delete and update actions work for direct SQL and survive
  close/reopen.
- Unsupported action shapes fail before MyLite catalog publication.
- Action side effects plus the parent row change remain statement-atomic.
- Docs and compatibility matrices distinguish this bounded support from full FK
  action compatibility.

## Risks And Open Questions

- The implementation scans child rows rather than using targeted child-index
  iteration. That is acceptable for this stage but should be revisited with
  broader action matrices and write-concurrency work.
- Opening a transient `TABLE` from catalog metadata must not let server-layer
  table cache state or durable sidecar behavior leak into the embedded
  lifecycle.
- Recursive action chains are intentionally not triggered. A later slice should
  define graph traversal, depth limits, lock ordering, and rollback semantics
  before claiming full action compatibility.
