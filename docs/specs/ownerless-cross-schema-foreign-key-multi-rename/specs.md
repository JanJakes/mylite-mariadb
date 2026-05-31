# Ownerless Cross-Schema Foreign-Key Multi-Rename

## Problem

Ownerless foreign-key rename coverage now proves same-schema and cross-schema
single-table parent/child renames, plus a same-schema multi-pair parent/child
rename. It still does not prove that one multi-pair `RENAME TABLE` statement
can move both sides of an InnoDB foreign-key relationship into another schema
while an already-open ownerless peer refreshes the final dictionary metadata,
generated constraint identity, native files, and enforcement behavior.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc` describes atomic table-list rename, where every
  two `TABLE_LIST` entries form one old/new rename pair.
- `mariadb/sql/sql_rename.cc` implements `mysql_rename_tables()` by taking
  exclusive metadata locks for the full rename list, pushing an internal error
  handler, and then calling `rename_tables()`. On failure, normal table renames
  are reverted through the DDL log.
- `rename_tables()` iterates each old/new pair and calls `do_rename()` for
  ordinary tables. `do_rename()` writes a DDL-log entry, calls
  `mysql_rename_table()`, and updates triggers/statistics for the pair.
- `mariadb/storage/innobase/handler/ha_innodb.cc` implements
  `ha_innobase::rename_table()`, which locks related foreign-key tables, the
  renamed table, InnoDB dictionary tables, and statistics tables before calling
  the InnoDB rename implementation.
- `mariadb/storage/innobase/row/row0mysql.cc` implements
  `row_rename_table_for_mysql()`. During rename it updates `SYS_TABLES`,
  updates `SYS_FOREIGN.FOR_NAME` for constraints owned by the renamed table,
  rewrites generated `_ibfk_` constraint IDs, updates matching
  `SYS_FOREIGN_COLS.ID` rows, and updates `SYS_FOREIGN.REF_NAME` for
  constraints that reference the renamed table. For cross-schema names these
  updates use normalized `db/table` identifiers.
- `mariadb/sql/sql_show.cc` exposes InnoDB foreign-key metadata through
  `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS`, including
  `CONSTRAINT_SCHEMA`, `UNIQUE_CONSTRAINT_SCHEMA`, `TABLE_NAME`, and
  `REFERENCED_TABLE_NAME`.

## Scope And Non-Goals

In scope:

- Cross-process ownerless SQL coverage for one multi-pair `RENAME TABLE`
  statement that moves a referenced parent table and the child table owning an
  unnamed foreign key from `app` into a target schema.
- Already-open ownerless peer refresh of the final parent schema/name, child
  schema/name, generated `<child>_ibfk_1` constraint name,
  `CONSTRAINT_SCHEMA`, `UNIQUE_CONSTRAINT_SCHEMA`, and referenced parent name
  after the single statement completes.
- Native parent and child `.frm`/`.ibd` movement from `datadir/app/` into the
  target schema directory.
- Valid child inserts, missing-parent rejection, and restricted-parent-delete
  rejection after the cross-schema multi-rename statement.
- Final ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Actual parent/child name swaps where the parent and child exchange table
  names and incompatible column layouts.
- Generated-column foreign keys and cyclic foreign-key graphs.
- Error-in-the-middle rollback injection or crash injection during the
  multi-rename statement.

## Design

- Add a focused `foreign-key-cross-schema-multi-rename` selector to
  `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates a target schema, a parent table in `app`,
  and a child table in `app`. The child table uses an unnamed foreign key so
  MariaDB/InnoDB generates the initial
  `ownerless_fk_cross_schema_multi_child_ibfk_1` constraint name.
- The parent keeps an already-open ownerless peer, verifies the initial
  parent/child metadata and generated constraint under `app`, and releases the
  child to execute one two-pair statement:
  `app.parent TO target.parent_moved, app.child TO target.child_moved`.
- After the child commits the statement, the peer verifies the old parent and
  child names are absent; target parent/child names are present;
  `REFERENTIAL_CONSTRAINTS` points from the moved child in the target schema to
  the moved parent in the target schema with the moved generated constraint
  name; and native files moved to the target schema directory.
- The peer inserts a valid child row through the moved child table, rejects a
  missing-parent child row with MariaDB errno 1452, and rejects deleting a
  referenced moved parent row with MariaDB errno 1451.
- Final state is verified through ownerless read/write and ordinary native
  exclusive reopen before and after deleting
  `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

This adds bounded ownerless evidence for MariaDB multi-pair `RENAME TABLE`
statements that move both sides of an InnoDB foreign-key relationship across
schemas. It does not claim complete FK rename graph coverage, generated-column
foreign keys, cyclic foreign-key graphs, or crash/error recovery inside the
statement.

## Directory And Lifecycle Impact

No new files or layout changes. The test verifies MariaDB native `.frm` and
`.ibd` files move from `datadir/app/` into the target schema directory and that
volatile shared-memory rebuild does not lose the final foreign-key metadata or
native files.

## Native Storage Impact

No storage-format changes. The slice exercises native InnoDB dictionary,
foreign-key metadata, generated constraint ID, and file-per-table rename
behavior across multiple cross-schema rename pairs in one SQL statement.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `foreign-key-cross-schema-multi-rename` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes the initial parent/child foreign key
  created in `app` by another ownerless process.
- After one cross-schema multi-pair `RENAME TABLE` statement, the peer observes
  target parent/child names and `REFERENTIAL_CONSTRAINTS` updated to the target
  child schema/table and target parent schema/table.
- Valid child inserts, missing-parent rejection, and restricted parent-delete
  rejection all use the moved tables.
- Final rows, metadata, and native files survive ownerless/native reopen before
  and after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves one cross-schema multi-pair parent/child FK rename shape.
  Generated-column foreign keys, cyclic foreign-key graphs, and crash/error
  injection during FK rename remain follow-up DDL/recovery work.
