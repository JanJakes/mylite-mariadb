# Ownerless Foreign-Key Multi-Rename Crash

## Problem

Ownerless same-schema foreign-key multi-rename coverage proves that an
already-open peer refreshes a parent table renamed through a temporary name and
the child table owning an unnamed foreign key, all in one `RENAME TABLE`
statement. Cross-schema FK multi-rename crash coverage separately proves the
post-native-DDL, pre-ownerless-finish boundary for schema moves.

The remaining bounded same-schema gap is a writer dying at
`dictionary-before-finish` after MariaDB completes the three-pair FK
multi-rename but before MyLite publishes the ownerless dictionary finish while
a live ownerless peer still prevents cleanup.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `mysql_rename_tables()` locks the complete rename list and delegates normal
    tables through `rename_tables()`.
  - `rename_tables()` processes each old/new pair and drives `do_rename()` for
    ordinary tables in the same `RENAME TABLE` statement.
- `mariadb/sql/sql_table.cc`
  - `mysql_rename_table()` calls the engine rename path and renames the `.frm`
    file for each normal-table pair.
- `mariadb/storage/innobase/handler/ha_innodb.cc`
  - `ha_innobase::rename_table()` coordinates related FK tables and InnoDB
    dictionary/statistics metadata before native rename.
- `mariadb/storage/innobase/row/row0mysql.cc`
  - `row_rename_table_for_mysql()` rewrites `SYS_TABLES`, `SYS_FOREIGN`
    owned/referenced names, generated `_ibfk_` constraint IDs, and
    `SYS_FOREIGN_COLS.ID` rows for renamed FK tables.
- `packages/libmylite/src/database.cc`
  - `ownerless_begin_dictionary_ddl()` marks ownerless dictionary DDL active
    before MariaDB executes the DDL.
  - `ownerless_finish_dictionary_ddl()` exposes `dictionary-before-finish`
    after native DDL success but before the ownerless dictionary generation is
    published.

## Scope And Non-Goals

In scope:

- Hook-build SQL coverage for a same-schema, three-pair parent/child
  foreign-key `RENAME TABLE`.
- Killing the writer at `dictionary-before-finish`.
- Verifying a live ownerless peer keeps cleanup busy until it exits.
- No-live ownerless recovery of final SQL metadata, InnoDB FK metadata,
  generated constraint identity, rows, and `.frm`/`.ibd` files.
- Verifying the temporary parent name is absent after recovery.
- Valid child insert, missing-parent rejection, and restricted parent-delete
  rejection after recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Crash injection inside MariaDB's native rename loop.
- Rename rollback after a later pair fails.
- Cross-schema FK multi-rename crash behavior, covered separately by
  `docs/specs/ownerless-cross-schema-foreign-key-multi-rename-crash/specs.md`.
- Randomized external FK graph or RQG execution.
- A complete durable DDL file-lifecycle protocol.

## Design

Add a hook-only `dictionary-foreign-key-multi-rename-crash` selector to
`mylite_ownerless_cross_process_sql_test`.

The selector creates a parent and child table in `app`; the child owns an
unnamed foreign key so MariaDB generates
`ownerless_fk_multi_rename_child_ibfk_1`. It holds a live ownerless peer and
kills a writer after this statement reaches `dictionary-before-finish`:

```sql
RENAME TABLE
  app.ownerless_fk_multi_rename_parent
    TO app.ownerless_fk_multi_rename_parent_tmp,
  app.ownerless_fk_multi_rename_child
    TO app.ownerless_fk_multi_rename_child_moved,
  app.ownerless_fk_multi_rename_parent_tmp
    TO app.ownerless_fk_multi_rename_parent_moved
```

After the peer exits, the test verifies old names and the temporary parent name
are absent, final parent/child names are present, `REFERENTIAL_CONSTRAINTS`
uses the moved generated constraint ID and moved parent/child names, native
`.frm`/`.ibd` files match the final names, and FK enforcement still rejects a
missing parent and a restricted parent delete.

No production code change is intended unless the selector exposes a recovery
failure.

## Compatibility Impact

No SQL syntax or public C API behavior changes. The ownerless hook-build DDL
crash evidence expands from FK ADD/DROP and cross-schema FK multi-rename crash
coverage to the same-schema parent-through-temporary FK multi-rename shape.
Overall DDL/file-lifecycle recovery remains partial.

## Directory And Lifecycle Impact

No directory layout changes. The selector verifies durable files remain inside
`datadir/app/` under the final moved parent/child names, the temporary parent
files are absent, and forced `concurrency/mylite-concurrency.shm` rebuild
preserves the recovered state.

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
- Run focused `dictionary-foreign-key-multi-rename-crash`.
- Run adjacent same-schema and cross-schema FK multi-rename crash CTest
  selectors.
- Run the product `foreign-key-multi-rename` and
  `foreign-key-cross-schema-multi-rename` selectors.
- Run format and diff checks.

## Acceptance Criteria

- A writer killed at `dictionary-before-finish` after the three-pair FK
  multi-rename leaves cleanup busy while a live peer remains.
- After no-live recovery, old parent/child names and the temporary parent name
  are absent, and moved parent/child names are present in SQL metadata, InnoDB
  metadata, and native files.
- Recovered FK metadata references the moved child and moved parent with the
  moved generated `<child>_ibfk_1` constraint identity.
- Post-recovery FK enforcement accepts a valid child row, rejects a missing
  parent with errno 1452, and rejects deleting a referenced parent with errno
  1451.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same final state.

## Risks And Open Questions

- This proves only the post-native-DDL, pre-ownerless-finish boundary.
- Broader DDL/file-lifecycle recovery, redo/checkpoint reconciliation, and
  randomized external DDL/FK oracle stress remain planned.
