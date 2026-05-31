# Ownerless Cross-Schema Rename

## Problem

Ownerless DDL coverage already proves same-schema table rename, schema
create/drop, and many InnoDB ALTER variants. It does not yet isolate the
`RENAME TABLE db1.t TO db2.t` path, where MariaDB must move SQL metadata and
the InnoDB file-per-table tablespace between schema directories while already
open ownerless peers refresh dictionary and page state.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc` implements `mysql_rename_tables()` for
  `RENAME TABLE`, takes exclusive metadata locks with `lock_table_names()`,
  records the operation in the DDL log, and delegates ordinary table rename to
  `mysql_rename_table()`.
- `mariadb/sql/sql_table.cc` implements `mysql_rename_table()`, builds old and
  new schema-qualified table filenames, calls the storage-engine
  `ha_rename_table()` path, then renames the `.frm` file.
- `mariadb/storage/innobase/handler/ha_innodb.cc` implements
  `ha_innobase::rename_table()`, starts a DDL transaction, locks the table,
  child foreign-key tables, InnoDB dictionary tables, and statistics tables,
  calls `innobase_rename_table()`, commits the transaction, and forces redo to
  the commit LSN.
- `mariadb/storage/innobase/row/row0mysql.cc` implements
  `row_rename_table_for_mysql()`, updates `SYS_TABLES`, foreign-key metadata,
  and then calls `dict_table_rename_in_cache()`.
- `mariadb/storage/innobase/dict/dict0dict.cc` documents that
  `dict_table_rename_in_cache()` also renames the `.ibd` file through
  `dict_table_t::rename_tablespace()`.

## Scope And Non-Goals

In scope:

- Cross-process ownerless SQL coverage for `RENAME TABLE app.t TO other.t`.
- Already-open ownerless peer refresh of source-table absence and target-table
  presence after the rename.
- Peer write through the renamed table after the cross-schema move.
- Native file movement checks for `.frm` and `.ibd` paths under `datadir/`.
- Final ownerless/native reopen checks before and after forced `.shm` rebuild.

Out of scope:

- Multi-table atomic rename cycles; the
  `ownerless-multi-rename-cycle` slice covers a same-schema swap separately.
- Cross-schema view rename, because MariaDB rejects schema changes for ordinary
  view rename.
- Trigger-file rename coverage, already handled by ownerless trigger DDL tests.
- Crash injection in the middle of cross-schema rename.
- Durable DDL file-lifecycle replay metadata beyond the existing conservative
  native-file bridge.

## Design

- Add a focused `cross-schema-rename` selector to
  `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates a target schema and an InnoDB table in
  `app`, inserts rows, then signals the parent.
- The parent keeps an already-open ownerless peer, verifies the source table and
  native source files exist, writes through the source table, and releases the
  child to rename the table into the target schema.
- After the child commits `RENAME TABLE`, the already-open parent verifies that
  the old table is absent, the target table is readable, `INFORMATION_SCHEMA`
  and InnoDB system table metadata name the target schema, and the source
  `.frm`/`.ibd` paths disappeared while target paths appeared.
- The parent writes through the moved table, closes peers, then verifies final
  state through ownerless read/write and ordinary native exclusive reopen before
  and after deleting `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

This extends ownerless DDL evidence from same-schema table rename and schema
lifecycle to cross-schema InnoDB table rename. It does not claim complete DDL
file-lifecycle recovery; crash-time replay for arbitrary table create/drop,
rename, truncate, discard/import, and partition file movement remains separate
work.

## Directory And Lifecycle Impact

No new files or layout changes. The test verifies MariaDB native files move
inside the MyLite database directory from `datadir/app/` to
`datadir/ownerless_rename_schema/` and that volatile shared-memory rebuild does
not lose the renamed table state.

## Native Storage Impact

No storage-format changes. The slice exercises the native InnoDB rename path,
including SQL metadata, InnoDB dictionary metadata, and the file-per-table
tablespace rename.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `cross-schema-rename` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes a table created by another process in
  `app`.
- That peer can write the source table before the child renames it.
- After `RENAME TABLE app.t TO ownerless_rename_schema.t2`, the peer sees the
  source table as absent and the target table as present/readable/writable.
- Source `.frm`/`.ibd` files are absent and target `.frm`/`.ibd` files are
  present under the MyLite database directory.
- Final metadata, rows, and native files survive ownerless/native reopen before
  and after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves one deterministic cross-schema rename shape, not all MariaDB
  multi-table rename atomicity behavior.
- Crash injection during rename and durable file-lifecycle replay metadata
  remain planned ownerless DDL/recovery work.
