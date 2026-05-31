# Ownerless Foreign-Key Child Rename

## Problem

Ownerless foreign-key coverage now proves enforcement, referential actions,
composite keys, `ALTER TABLE` add/drop refresh, and parent-table rename
refresh. It still does not prove that the table owning a foreign-key constraint
can be renamed by one ownerless process while an already-open peer refreshes
the child table name, generated InnoDB foreign-key constraint ID, native files,
and enforcement state.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc` implements
  `ha_innobase::rename_table()`. It opens the source table, locks child
  foreign-key tables, locks the table being renamed, locks InnoDB dictionary and
  statistics tables, and then calls the InnoDB rename implementation.
- `mariadb/storage/innobase/row/row0mysql.cc` implements
  `row_rename_table_for_mysql()`. During rename it updates `SYS_TABLES` and, for
  constraints owned by the renamed table, updates `SYS_FOREIGN.FOR_NAME`.
- The same InnoDB path rewrites generated constraint IDs whose prefix is the
  old table name plus `_ibfk_`, updates matching `SYS_FOREIGN_COLS.ID` rows, and
  updates `SYS_FOREIGN.REF_NAME` for constraints that reference the renamed
  table.
- After `dict_table_rename_in_cache()` renames the cached dictionary table and
  native `.ibd`, InnoDB calls `dict_load_foreigns()` so related foreign-key
  tables are visible under the new dictionary name.

## Scope And Non-Goals

In scope:

- Cross-process ownerless SQL coverage for renaming a child table that owns an
  InnoDB foreign key.
- An unnamed foreign key so the generated `<child>_ibfk_1` constraint ID must be
  renamed with the child table.
- Already-open ownerless peer refresh of `REFERENTIAL_CONSTRAINTS` from the old
  child table and generated constraint name to the new child table and
  generated constraint name.
- Valid child inserts, missing-parent rejection, and restricted-parent-delete
  rejection after the child table rename.
- Native child-table `.frm`/`.ibd` movement checks and final ownerless/native
  reopen before and after forced `.shm` rebuild.

Out of scope:

- Cross-schema child-table rename with foreign keys; that shape is covered
  separately by
  `docs/specs/ownerless-cross-schema-foreign-key-child-rename/specs.md`.
- Parent-table rename, already covered by
  `ownerless-foreign-key-rename`.
- Multi-table parent/child rename involving foreign-key tables; the
  same-schema shape is covered separately by
  `docs/specs/ownerless-foreign-key-multi-rename/specs.md`.
- Supported stored generated-column foreign keys, covered separately by
  `docs/specs/ownerless-generated-column-foreign-key/specs.md`.
- Unsupported generated-column FK variants and cyclic foreign-key graphs.
- Crash injection during the child-table rename.

## Design

- Add a focused `foreign-key-child-rename` selector to
  `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates parent and child InnoDB tables. The child
  table uses an unnamed foreign key so MariaDB/InnoDB generates the
  `ownerless_fk_child_rename_child_ibfk_1` constraint name.
- The parent keeps an already-open ownerless peer, verifies the original child
  table name and generated constraint name, and releases the child to rename the
  child table.
- After the child commits `RENAME TABLE`, the peer verifies source-child
  absence, target-child presence, generated constraint-name refresh,
  `REFERENTIAL_CONSTRAINTS.TABLE_NAME` refresh, unchanged referenced parent
  table, and native file movement.
- The peer inserts a valid child row through the renamed child table, rejects a
  missing-parent child row with MariaDB errno 1452, and rejects deleting a
  referenced parent row with MariaDB errno 1451.
- Final state is verified through ownerless read/write and ordinary native
  exclusive reopen before and after deleting
  `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

This adds bounded ownerless evidence for same-schema InnoDB foreign-key
metadata updates when the table owning the constraint is renamed. It does not
claim full foreign-key rename graph coverage or crash-in-action recovery.

## Directory And Lifecycle Impact

No new files or layout changes. The test verifies the child table's native
metadata (`.frm`) and `.ibd` files move within `datadir/app/`, while the parent
table and renamed foreign-key metadata remain durable across volatile
shared-memory rebuild.

## Native Storage Impact

No storage-format changes. The slice relies on MariaDB/InnoDB native
foreign-key dictionary rename behavior and MyLite's existing ownerless DDL
generation, page visibility, and reopen paths.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `foreign-key-child-rename` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes the initial foreign key owned by the
  original child table with the original generated constraint name.
- After child `RENAME TABLE`, the peer observes the old child table as absent,
  the new child table as present, and the generated constraint name updated to
  the renamed child table.
- Valid child inserts, missing-parent rejection, and restricted parent-delete
  rejection all use the renamed child table and unchanged parent table.
- Final rows, metadata, and native child files survive ownerless/native reopen
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves one same-schema child-table rename shape. Unsupported
  generated-column FK variants, cyclic foreign-key graphs, and crash injection
  during FK rename remain follow-up DDL/recovery work.
