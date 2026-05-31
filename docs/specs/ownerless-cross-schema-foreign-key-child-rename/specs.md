# Ownerless Cross-Schema Foreign-Key Child Rename

## Problem

Ownerless foreign-key rename coverage proves same-schema parent and child table
rename refresh, and cross-schema parent-table rename refresh. It still does not
prove that the table owning an InnoDB foreign-key constraint can move to another
schema while an already-open ownerless peer refreshes the owning constraint
schema, generated constraint identity, native schema-directory file lifecycle,
and enforcement state.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc` implements
  `ha_innobase::rename_table()`. It opens the source table, locks child
  foreign-key tables, locks the table being renamed, locks InnoDB dictionary and
  statistics tables, and calls the InnoDB rename implementation.
- `mariadb/storage/innobase/row/row0mysql.cc` implements
  `row_rename_table_for_mysql()`. During rename it updates `SYS_TABLES` and, for
  constraints owned by the renamed table, updates `SYS_FOREIGN.FOR_NAME`.
- The same InnoDB path rewrites generated constraint IDs whose prefix is the old
  normalized `db/table` name plus `_ibfk_`. For cross-schema child renames this
  changes the generated constraint ID from `app/old_child_ibfk_N` to
  `target_schema/new_child_ibfk_N`, and updates matching `SYS_FOREIGN_COLS.ID`
  rows.
- For constraints that reference a renamed table, the path updates
  `SYS_FOREIGN.REF_NAME`; this slice instead keeps the parent table stable and
  exercises the owning child-table path.
- `mariadb/sql/sql_show.cc` exposes InnoDB foreign-key metadata through
  `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS`, including the owning constraint
  schema, referenced unique-constraint schema, table name, referenced table
  name, and rules.

## Scope And Non-Goals

In scope:

- Cross-process ownerless SQL coverage for renaming a child table that owns an
  InnoDB foreign key from `app` into another schema.
- An unnamed foreign key so the generated `<child>_ibfk_1` constraint ID must
  move from the old schema/table prefix to the target schema/table prefix.
- Already-open ownerless peer refresh of `REFERENTIAL_CONSTRAINTS` so
  `CONSTRAINT_SCHEMA`, `CONSTRAINT_NAME`, and `TABLE_NAME` move to the target
  schema/table while `UNIQUE_CONSTRAINT_SCHEMA` and `REFERENCED_TABLE_NAME`
  continue to identify the unchanged parent table.
- Valid child inserts, missing-parent rejection, and restricted-parent-delete
  rejection after the cross-schema child rename.
- Native child-table `.frm`/`.ibd` movement between schema directories and
  final ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Cross-schema parent-table rename with foreign keys, already covered by
  `docs/specs/ownerless-cross-schema-foreign-key-rename/specs.md`.
- Cross-schema parent/child multi-rename in one statement, covered separately
  by
  `docs/specs/ownerless-cross-schema-foreign-key-multi-rename/specs.md`.
- Supported stored generated-column foreign keys, covered separately by
  `docs/specs/ownerless-generated-column-foreign-key/specs.md`.
- Unsupported generated-column FK variants and cyclic foreign-key graphs.
- Crash injection during the cross-schema rename.

## Design

- Add a focused `foreign-key-cross-schema-child-rename` selector to
  `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates a target schema, a parent table in `app`,
  and a child table in `app` with an unnamed child-to-parent foreign key.
- The parent keeps an already-open ownerless peer, verifies the initial
  generated constraint under `app`, and releases the child to rename the child
  table into the target schema.
- After the child commits `RENAME TABLE`, the peer verifies source-child
  absence, target-child presence, generated constraint-name refresh,
  `REFERENTIAL_CONSTRAINTS.CONSTRAINT_SCHEMA` and `TABLE_NAME` movement, stable
  parent metadata, cross-schema native file movement, valid child insert,
  missing-parent rejection with MariaDB errno 1452, and restricted parent-delete
  rejection with MariaDB errno 1451.
- Final state is verified through ownerless read/write and ordinary native
  exclusive reopen before and after deleting
  `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

This adds bounded ownerless evidence for cross-schema InnoDB foreign-key child
rename. It does not claim full FK graph coverage, unsupported
generated-column FK variants, cyclic foreign-key graph coverage, or
crash-in-action recovery.

## Directory And Lifecycle Impact

No new files or layout changes. The test verifies the child table's native
metadata (`.frm`) and `.ibd` files move from `datadir/app/` into the target
schema directory, while the parent table remains in `datadir/app/` and the
foreign-key metadata survives volatile shared-memory rebuild.

## Native Storage Impact

No storage-format changes. The slice relies on MariaDB/InnoDB native
foreign-key dictionary rename behavior and MyLite's existing ownerless DDL
generation, page visibility, and reopen paths.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `foreign-key-cross-schema-child-rename` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes the initial generated foreign key
  owned by the child table in `app`.
- After cross-schema child `RENAME TABLE`, the peer observes the old child table
  as absent, the target-schema child table as present, and the generated
  constraint moved to the target schema/table.
- Valid child inserts, missing-parent rejection, and restricted parent-delete
  rejection all use the moved child table and unchanged parent table.
- Final rows, metadata, and native child files survive ownerless/native reopen
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves one cross-schema child-table FK rename shape. Unsupported
  generated-column FK variants, cyclic foreign-key graphs, and crash injection
  during FK rename remain follow-up DDL/recovery work.
