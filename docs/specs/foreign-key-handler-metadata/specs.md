# Foreign-Key Handler Metadata

## Goal

Expose MyLite-owned foreign-key metadata through MariaDB handler metadata hooks
without enabling public foreign-key SQL, referential checks, or cascade actions.
This slice should let the MariaDB layer inspect stored child and parent FK
relationships from the primary `.mylite` file, so later DDL and enforcement
slices do not depend on InnoDB's dictionary or persistent sidecars.

## Non-Goals

- Accepting `CREATE TABLE` or `ALTER TABLE` foreign-key DDL through
  `libmylite`.
- Advertising `HTON_SUPPORTS_FOREIGN_KEYS` from the MyLite handlerton.
- Enforcing child/parent row existence checks, restrict checks, cascading
  actions, or `foreign_key_checks=0` import semantics.
- Implementing statement-scoped table-plus-FK publication for inline
  `CREATE TABLE` constraints.
- Implementing hot-path parent lookup indexes or FK-lock integration.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/handler.h:handler` defines the FK metadata hooks:
  `is_fk_defined_on_table_or_index()`, `get_foreign_key_create_info()`,
  `get_foreign_key_list()`, `get_parent_foreign_key_list()`,
  `referenced_by_foreign_key()`, and `free_foreign_key_create_info()`.
- `mariadb/sql/handler.h:HTON_SUPPORTS_FOREIGN_KEYS` gates engine-level FK
  support advertisement. MyLite must keep this bit clear until metadata,
  DDL publication, row enforcement, locking, and recovery agree.
- `mariadb/sql/table.h:FOREIGN_KEY_INFO` is the server-facing metadata object.
  It stores child and parent identifiers, column lists, referenced key name,
  update/delete actions, and child/parent nullable bits.
- `mariadb/sql/sql_show.cc` uses `get_foreign_key_list()` to populate
  `INFORMATION_SCHEMA.TABLE_CONSTRAINTS`, `KEY_COLUMN_USAGE`, and
  `REFERENTIAL_CONSTRAINTS`.
- `mariadb/sql/sql_table.cc:fk_prepare_copy_alter_table()` uses child and
  parent FK lists when deciding whether copy ALTER may drop, rename, or change
  FK columns.
- `mariadb/sql/sql_table.cc` performs copy ALTER by renaming the old table to
  an internal `#sql-backup-*` name, renaming the rebuilt temporary table to the
  logical table name, and then dropping the backup. MyLite must not move FK
  catalog records onto the internal backup identity during that first rename.
- `mariadb/sql/sql_truncate.cc` and `mariadb/sql/sql_base.cc` use
  `referenced_by_foreign_key()` and `get_parent_foreign_key_list()` for
  parent-table checks.
- `mariadb/storage/innobase/handler/ha_innodb.cc:get_foreign_key_info()` maps
  native InnoDB dictionary objects into `FOREIGN_KEY_INFO`. MyLite should copy
  the handler-contract shape, not the InnoDB dictionary dependency.

## Compatibility Impact

Public SQL compatibility remains unchanged: `libmylite` continues rejecting
foreign-key DDL before MariaDB execution, including `ENGINE=InnoDB` tables.
This slice changes internal metadata visibility for MyLite-seeded FK records
only. The compatibility matrix should continue to mark foreign keys partial and
state that MariaDB-visible metadata hooks are groundwork, not referential
integrity support.

## Design

Add a storage parent-list primitive:

- `mylite_storage_list_parent_foreign_keys(filename, referenced_schema,
  referenced_table, callback, ctx)`.

The new primitive scans FK catalog records whose parent schema/table match the
requested table, decodes the existing FK metadata blob, and invokes the same
callback shape used by `mylite_storage_list_foreign_keys()`. This keeps parent
metadata lookup inside `packages/mylite-storage` instead of duplicating catalog
record parsing in the MariaDB handler.

Then add MyLite handler helpers that map `mylite_storage_foreign_key_metadata`
to `FOREIGN_KEY_INFO` allocated from `THD` memory. The mapping must preserve:

- child schema/table and constraint name,
- parent schema/table and referenced key name,
- child and referenced column order,
- update/delete actions,
- child and referenced nullable-column bitmaps.

`ha_mylite::get_foreign_key_list()` reads child FK metadata for
`storage_schema()` / `storage_table()`. `ha_mylite::get_parent_foreign_key_list()`
reads parent FK metadata from the new storage parent-list primitive.
`ha_mylite::referenced_by_foreign_key()` can use the parent-list primitive with
a stop-after-first callback. Errors should fail closed through the list hooks'
handler error codes and by returning `true` from the `noexcept` boolean hook
when storage cannot be inspected.

`get_foreign_key_create_info()` is implemented by the follow-up
`foreign-key-create-info` slice. `is_fk_defined_on_table_or_index()` stays
conservative, while SQL copy ALTER validates retained child and parent FK
metadata against the prepared post-ALTER key list so FK columns and required
supporting keys cannot be removed underneath manually seeded metadata.

During copy ALTER, MyLite uses a rebuild-backup rename path for MariaDB's
internal old-table backup rename. It renames only the table record while
leaving child and parent FK records attached to the logical table identity that
the rebuilt temporary table will take over. Ordinary `RENAME TABLE` still
rewrites child and parent FK identities.

## File Lifecycle

No new files are introduced. FK metadata remains durable only inside the
primary `.mylite` file, backed by the catalog and typed FK blob pages from the
storage-metadata slice. Parent-list scans use existing storage read locking and
recovery validation. No `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`,
`aria_log.*`, binlog, relay-log, or plugin-owned durable file is introduced.

## Embedded Lifecycle And API

No public `libmylite` API is added. The storage API grows by one internal
first-party FK listing function. Open/close ownership is unchanged: handler
metadata hooks read from the primary file configured for the active embedded
runtime. Direct and prepared FK DDL must continue returning the existing
unsupported-surface diagnostic.

## Build, Size, And Dependencies

No dependency is introduced. The MariaDB fork delta is limited to the MyLite
handler files under `mariadb/storage/mylite/`. Binary-size impact is expected
to be small and limited to storage scanning plus handler mapping helpers; no
native InnoDB dictionary or FK code is reintroduced.

## Test Plan

- Storage unit coverage for listing FK metadata by referenced parent table.
- Storage unit coverage that parent-list records survive parent and child
  renames.
- Handler/storage-smoke coverage that manually seeded FK metadata appears in
  MariaDB information-schema FK metadata rows before public FK DDL
  publication.
- Handler/storage-smoke coverage that public FK DDL did not become accepted
  before catalog publication existed. The supported public subset is now
  covered by
  [Foreign-Key DDL Publication](../foreign-key-ddl-publication/specs.md).
- Format check, storage build, storage unit test, storage-smoke build/test when
  handler code changes, full default `ctest --preset dev`, and `git diff
  --check`.

## Acceptance Criteria

- Child FK metadata hooks return the stored FK records for the opened MyLite
  table.
- Parent FK metadata hooks return stored child records that reference the
  opened MyLite table.
- `FOREIGN_KEY_INFO` fields match stored identifiers, column order, actions,
  referenced key name, and nullable bits.
- Retained FK metadata survives covered copy ALTER rebuilds and rejects
  copy-ALTER attempts to remove FK columns or the last supporting child/parent
  key.
- `HTON_SUPPORTS_FOREIGN_KEYS` remains clear and FK DDL remains rejected.
- FK metadata remains single-file and does not rely on InnoDB dictionary state
  or persistent MariaDB sidecars.

## Risks And Open Questions

- `referenced_by_foreign_key()` has only a boolean result and is `noexcept`, so
  storage errors cannot be fully reported through that specific hook. The list
  hooks must carry the actionable errors for DDL paths that can return an error
  code.
- Parent FK lookup is a catalog scan. That is acceptable for metadata paths but
  must be replaced or cached before FK enforcement becomes hot DML behavior.
- Public FK DDL needs statement-scoped table-plus-FK publication before inline
  `CREATE TABLE` FK clauses can be accepted safely.
- FK-aware ALTER checks currently cover manually seeded metadata only; public
  FK DDL still needs statement-scoped publication before these checks can
  become user-visible FK support.
