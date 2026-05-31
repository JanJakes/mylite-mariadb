# Ownerless Foreign-Key Rename

## Problem

Ownerless foreign-key coverage proves enforcement, referential actions,
composite keys, and `ALTER TABLE` add/drop refresh. Ownerless rename coverage
proves table file movement and multi-pair rename cycles. It still does not
prove that a parent table referenced by an InnoDB foreign key can be renamed by
one process while an already-open ownerless peer refreshes the updated
foreign-key metadata and continues enforcing the constraint through the new
parent table name.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc` implements
  `ha_innobase::rename_table()`, locks the table, child foreign-key tables,
  InnoDB dictionary tables, and statistics tables before calling the InnoDB
  rename implementation.
- `mariadb/storage/innobase/row/row0mysql.cc` implements
  `row_rename_table_for_mysql()`. During rename it updates `SYS_TABLES`, then
  updates `SYS_FOREIGN.FOR_NAME` for constraints owned by the renamed table and
  `SYS_FOREIGN.REF_NAME` for constraints that reference the renamed table.
- The same InnoDB path updates generated constraint IDs when the renamed table
  name forms the foreign-key ID prefix and calls `dict_load_foreigns()` after
  `dict_table_rename_in_cache()` so related foreign-key tables are loaded under
  the new name.
- `mariadb/sql/sql_show.cc` exposes the updated foreign-key dictionary through
  `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS`, which existing ownerless FK DDL
  tests already use as a peer-refresh oracle.

## Scope And Non-Goals

In scope:

- Cross-process ownerless SQL coverage for renaming a referenced parent table.
- Already-open ownerless peer refresh of `REFERENTIAL_CONSTRAINTS` from the old
  referenced table name to the new table name.
- Valid child inserts through the renamed parent table.
- Missing-parent and restricted-parent-delete enforcement after the rename.
- Native `.frm`/`.ibd` movement checks and final ownerless/native reopen before
  and after forced `.shm` rebuild.

Out of scope:

- Renaming the child table that owns the foreign-key constraint; that
  same-schema shape is covered separately by
  `docs/specs/ownerless-foreign-key-child-rename/specs.md`.
- Cross-schema parent-table rename with foreign keys; that shape is covered
  separately by
  `docs/specs/ownerless-cross-schema-foreign-key-rename/specs.md`.
- Multi-table parent/child rename involving foreign-key tables; the
  same-schema shape is covered separately by
  `docs/specs/ownerless-foreign-key-multi-rename/specs.md`.
- Crash injection during the parent-table rename.

## Design

- Add a focused `foreign-key-rename` selector to
  `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates parent and child InnoDB tables with an
  explicit foreign-key constraint and inserts parent/child rows.
- The parent keeps an already-open ownerless peer, verifies the constraint
  references the original parent name, and releases the child to rename the
  parent table.
- After the child commits `RENAME TABLE`, the peer verifies source-table
  absence, target-table presence, updated `REFERENTIAL_CONSTRAINTS`, and native
  file movement.
- The peer inserts a valid child row that references the renamed parent, rejects
  a missing-parent child row with MariaDB errno 1452, and rejects deleting a
  referenced parent row with MariaDB errno 1451.
- The final state is verified through ownerless read/write and ordinary native
  exclusive reopen before and after deleting
  `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

This adds bounded ownerless evidence for InnoDB foreign-key metadata updates
during parent-table rename. It does not claim full rename coverage for all
foreign-key table graphs or crash-in-action recovery.

## Directory And Lifecycle Impact

No new files or layout changes. The test verifies the parent table's native
metadata (`.frm`) and `.ibd` files move within `datadir/app/`, while the child
table and foreign-key metadata remain durable across volatile shared-memory
rebuild.

## Native Storage Impact

No storage-format changes. The slice relies on MariaDB/InnoDB native
foreign-key dictionary rename behavior and MyLite's existing ownerless DDL
generation, page visibility, and reopen paths.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `foreign-key-rename` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes the initial foreign key referencing
  the original parent table name.
- After parent `RENAME TABLE`, the peer observes the old parent table as absent,
  the new parent table as present, and the foreign key referencing the new
  parent table name.
- Valid child inserts, missing-parent rejection, and restricted parent-delete
  rejection all use the renamed parent table.
- Final rows, metadata, and native parent files survive ownerless/native reopen
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves one same-schema parent-table rename shape. Generated-column
  foreign keys, cyclic/deep cascade chains, and crash injection during FK
  rename remain follow-up DDL/recovery work.
