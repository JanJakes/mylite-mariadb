# Ownerless Cross-Schema Foreign-Key Multi-Rename Crash

## Problem

Ownerless foreign-key multi-rename coverage proves that an already-open peer
refreshes a parent/child foreign-key relationship moved from `app` into another
schema by one `RENAME TABLE` statement. Separate hook coverage kills non-FK
cross-schema multi-rename writers after MariaDB completes native file and
dictionary changes but before MyLite publishes the ownerless dictionary finish.

The remaining bounded gap is their intersection: a cross-schema multi-pair
foreign-key rename whose native InnoDB metadata and file moves complete, but
the writer dies at `dictionary-before-finish` while a live ownerless peer keeps
cleanup busy.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `mysql_rename_tables()` acquires exclusive metadata locks for the full
    rename list and drives normal tables through `rename_tables()`.
  - `rename_tables()` processes each old/new pair in the same `RENAME TABLE`
    statement and calls `do_rename()` for ordinary tables.
- `mariadb/sql/sql_table.cc`
  - `mysql_rename_table()` builds the schema-qualified old/new file names,
    invokes the storage-engine rename path, and renames the `.frm` metadata
    file.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::rename_table()` coordinates related foreign-key tables,
    InnoDB dictionary state, and statistics metadata before the native rename.
- `mariadb/storage/innobase/row/row0mysql.cc`
  - `row_rename_table_for_mysql()` updates `SYS_TABLES`, owned
    `SYS_FOREIGN.FOR_NAME`, referenced `SYS_FOREIGN.REF_NAME`, generated
    `_ibfk_` constraint IDs, and matching `SYS_FOREIGN_COLS.ID` rows using
    normalized `db/table` identifiers.
- `packages/libmylite/src/database.cc`
  - `ownerless_begin_dictionary_ddl()` marks dictionary DDL active before
    MariaDB executes the DDL.
  - `ownerless_finish_dictionary_ddl()` exposes the unsafe
    `dictionary-before-finish` hook after native DDL success but before the
    ownerless dictionary generation is published.

## Scope And Non-Goals

In scope:

- Hook-build SQL coverage for a cross-schema, two-pair parent/child
  foreign-key `RENAME TABLE`.
- Killing the writer at `dictionary-before-finish`.
- Verifying a live ownerless peer keeps cleanup busy until it exits.
- No-live ownerless recovery of final SQL metadata, InnoDB foreign-key
  metadata, generated constraint identity, rows, and `.frm`/`.ibd` files.
- Valid child insert, missing-parent rejection, and restricted parent-delete
  rejection after recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Crash injection inside MariaDB's native rename loop.
- Rename rollback after a later pair fails.
- Same-schema parent-through-temporary foreign-key multi-rename crash coverage.
- Randomized external FK graph or RQG execution.
- A complete durable DDL file-lifecycle protocol.

## Design

Add a hook-only
`dictionary-foreign-key-cross-schema-multi-rename-crash` selector to
`mylite_ownerless_cross_process_sql_test`.

The selector creates `ownerless_fk_cross_schema_multi_schema`, a parent table in
`app`, and a child table in `app` with an unnamed foreign key so MariaDB
generates `ownerless_fk_cross_schema_multi_child_ibfk_1`. It holds a live
ownerless peer and kills a writer after this statement reaches
`dictionary-before-finish`:

```sql
RENAME TABLE
  app.ownerless_fk_cross_schema_multi_parent
    TO ownerless_fk_cross_schema_multi_schema.ownerless_fk_cross_schema_multi_parent_moved,
  app.ownerless_fk_cross_schema_multi_child
    TO ownerless_fk_cross_schema_multi_schema.ownerless_fk_cross_schema_multi_child_moved
```

After the peer exits, the test verifies the old `app` names are absent, the
target-schema names are present, `REFERENTIAL_CONSTRAINTS` uses target
`CONSTRAINT_SCHEMA` and `UNIQUE_CONSTRAINT_SCHEMA`, the generated constraint ID
is rewritten to the moved child table, native `.frm`/`.ibd` files moved to the
target schema directory, and FK enforcement still rejects a missing parent and
a restricted parent delete.

No production code change is intended unless the selector exposes a recovery
failure.

## Compatibility Impact

No SQL syntax or public C API behavior changes. The ownerless hook-build DDL
crash evidence expands from non-FK cross-schema multi-rename and FK
cross-schema multi-rename peer refresh to the post-native-DDL,
pre-ownerless-finish crash boundary for the FK shape. Overall DDL/file-lifecycle
recovery remains partial.

## Directory And Lifecycle Impact

No directory layout changes. The selector verifies durable files remain inside
the MyLite database directory, moving from `datadir/app/` to
`datadir/ownerless_fk_cross_schema_multi_schema/`, and that forced
`concurrency/mylite-concurrency.shm` rebuild preserves the recovered state.

## Native Storage Impact

No storage-format changes. MariaDB native InnoDB dictionary metadata and
file-per-table rename state remain the recovery authority after ownerless
dictionary finish is interrupted.

## Embedded Lifecycle And API

No public API changes. The slice covers embedded ownerless open/close behavior
around a killed DDL writer, a live peer, no-live recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen.

## Build, Size, And Dependencies

No production binary-size, dependency, or license impact. The change adds
hook-build test code, a CTest entry, and documentation only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused `dictionary-foreign-key-cross-schema-multi-rename-crash`.
- Run adjacent FK/dictionary crash CTest selectors.
- Run or attempt the broader hook crash tail and record any unrelated selector
  failure separately from this slice's focused evidence.
- Run the product `foreign-key-cross-schema-multi-rename` selector.
- Run format and diff checks.

## Acceptance Criteria

- A writer killed at `dictionary-before-finish` after the cross-schema FK
  multi-rename leaves cleanup busy while a live peer remains.
- After no-live recovery, old parent/child names are absent and moved
  parent/child names are present in SQL metadata, InnoDB metadata, and native
  files.
- Recovered FK metadata references the moved child and moved parent in the
  target schema with the moved generated `<child>_ibfk_1` constraint identity.
- Post-recovery FK enforcement accepts a valid child row, rejects a missing
  parent with errno 1452, and rejects deleting a referenced parent with errno
  1451.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same final state.

## Risks And Open Questions

- This proves only the post-native-DDL, pre-ownerless-finish boundary.
- Same-schema parent-through-temporary FK multi-rename crash coverage remains a
  separate focused follow-up.
- Broader DDL/file-lifecycle recovery, redo/checkpoint reconciliation, and
  randomized external DDL/FK oracle stress remain planned.
