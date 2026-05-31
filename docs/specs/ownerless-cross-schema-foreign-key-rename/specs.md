# Ownerless Cross-Schema Foreign-Key Rename

## Problem

Ownerless rename coverage proves cross-schema movement for ordinary InnoDB
tables, and foreign-key rename coverage proves same-schema parent and child
table rename refresh. It still does not prove that a referenced parent table can
move to another schema while an already-open ownerless peer refreshes both the
foreign-key dictionary and the native schema-directory file lifecycle.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc` implements
  `ha_innobase::rename_table()`. The path locks child foreign-key tables, the
  table being renamed, InnoDB dictionary tables, and statistics tables before
  calling the InnoDB rename implementation.
- `mariadb/storage/innobase/row/row0mysql.cc` implements
  `row_rename_table_for_mysql()`. During rename it updates `SYS_TABLES`, updates
  `SYS_FOREIGN.FOR_NAME` for constraints owned by the renamed table, and updates
  `SYS_FOREIGN.REF_NAME` for constraints that reference the renamed table.
- The InnoDB rename path works on normalized `db/table` names, so moving a
  parent table across schemas is represented as a dictionary name update from
  `app/parent` to `other_schema/parent`.
- `mariadb/sql/sql_show.cc` exposes InnoDB foreign-key metadata through
  `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS`, including the owning constraint
  schema, referenced unique-constraint schema, table name, referenced table
  name, and rules.

## Scope And Non-Goals

In scope:

- Cross-process ownerless SQL coverage for renaming a referenced parent table
  from `app` into another schema.
- Already-open ownerless peer refresh of `REFERENTIAL_CONSTRAINTS` so the child
  remains in `app` while `UNIQUE_CONSTRAINT_SCHEMA` and
  `REFERENCED_TABLE_NAME` move to the target schema/table.
- Valid child inserts through the moved cross-schema parent table.
- Missing-parent and restricted-parent-delete enforcement after the
  cross-schema rename.
- Native parent-table `.frm`/`.ibd` movement between schema directories and
  final ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Cross-schema rename of the child table that owns the foreign-key constraint;
  that shape is covered separately by
  `docs/specs/ownerless-cross-schema-foreign-key-child-rename/specs.md`.
- Moving parent and child together in one multi-table rename statement.
- Generated-column foreign keys and cyclic/deep cascade chains.
- Crash injection during the cross-schema rename.

## Design

- Add a focused `foreign-key-cross-schema-rename` selector to
  `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates a target schema, a parent table in `app`,
  and a child table in `app` with an explicit child-to-parent foreign key.
- The parent keeps an already-open ownerless peer, verifies the initial
  constraint references `app.ownerless_fk_cross_schema_parent`, and releases the
  child to rename the parent into the target schema.
- After the child commits `RENAME TABLE`, the peer verifies source-table
  absence, target-table presence, `REFERENTIAL_CONSTRAINTS` refresh,
  cross-schema native file movement, valid child insert, missing-parent
  rejection with MariaDB errno 1452, and restricted parent-delete rejection with
  MariaDB errno 1451.
- Final state is verified through ownerless read/write and ordinary native
  exclusive reopen before and after deleting
  `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

This adds bounded ownerless evidence for cross-schema InnoDB foreign-key parent
rename. It does not claim full cross-schema FK graph coverage, multi-table FK
rename cycles, or crash-in-action recovery.

## Directory And Lifecycle Impact

No new files or layout changes. The test verifies the parent table's native
metadata (`.frm`) and `.ibd` files move from `datadir/app/` into the target
schema directory, while the child table and foreign-key metadata remain durable
across volatile shared-memory rebuild.

## Native Storage Impact

No storage-format changes. The slice relies on MariaDB/InnoDB native
foreign-key dictionary rename behavior and MyLite's existing ownerless DDL
generation, page visibility, and reopen paths.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `foreign-key-cross-schema-rename` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes the initial foreign key referencing
  the parent table in `app`.
- After cross-schema parent `RENAME TABLE`, the peer observes the old parent
  table as absent, the target-schema parent as present, and the foreign key
  referencing the target schema/table.
- Valid child inserts, missing-parent rejection, and restricted parent-delete
  rejection all use the moved parent table.
- Final rows, metadata, and native parent files survive ownerless/native reopen
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves one cross-schema parent-table FK rename shape. Parent/child
  multi-table rename cycles, generated-column foreign keys, cyclic/deep cascade
  chains, and crash injection during FK rename remain follow-up DDL/recovery
  work.
