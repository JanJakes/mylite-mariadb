# Metadata And DDL Lifecycle

## Goal

Prove controlled MariaDB DDL metadata and native MyISAM engine files stay inside
the MyLite database directory across create, alter, rename, drop, close, and
reopen.

## Non-Goals

- Do not broaden support beyond controlled MyISAM DDL lifecycle coverage.
- Do not enable or claim InnoDB or Aria table lifecycle support yet.
- Do not claim transaction, crash-recovery, locking, or concurrency guarantees.
- Do not implement a custom metadata catalog.
- Do not change the public `libmylite` API.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_db.cc:748-818` creates schema directories under the data
  home and writes `db.opt`.
- `mariadb/sql/sql_db.cc:1059-1161` drops schema contents and deletes `db.opt`.
- `mariadb/sql/sql_table.cc:5001-5040` builds table paths from the data home
  before calling the native create-table implementation.
- `mariadb/sql/sql_table.cc:5544-5605` renames table handler files and `.frm`
  metadata through MariaDB's native rename path.
- `mariadb/sql/sql_table.cc:12123-12205` uses native rename/delete phases to
  complete copy-style `ALTER TABLE` operations.
- `mariadb/sql/handler.cc:3353-3368` routes table deletion through the selected
  handlerton.
- `mariadb/sql/handler.cc:5350-5402` implements default engine-file
  delete/rename behavior for handler extensions.
- `mariadb/storage/myisam/ha_myisam.cc:2197-2199` deletes MyISAM tables through
  `mi_delete_table()`.
- `mariadb/storage/myisam/ha_myisam.cc:2329-2331` renames MyISAM table files
  through `mi_rename()`.

These paths use the configured MariaDB data directory. MyLite already points
that data directory at `<db>.mylite/datadir`, so this slice should be mostly
test and documentation coverage unless the boundary assertions expose an
escaping file.

## Compatibility Impact

This slice strengthens the documented MyISAM and metadata lifecycle surface from
simple create/insert/select persistence to controlled `CREATE DATABASE`,
`CREATE TABLE`, `ALTER TABLE`, `RENAME TABLE`, `DROP TABLE`, and
`DROP DATABASE` coverage. The result is still partial compatibility because it
does not cover all DDL forms, engines, triggers, views, routines, partitions,
or crash recovery.

## Design

Add an embedded integration test that opens one durable `.mylite/` directory,
executes the controlled DDL sequence through `mylite_exec()`, verifies the
native files inside `datadir/`, reopens the directory, reads the altered table,
then drops the table and schema.

The slice should not add a MyLite metadata abstraction. MariaDB remains the
metadata authority for schema directories, `db.opt`, `.frm`, and native MyISAM
files while MyLite owns the directory boundary and runtime lifecycle.

## File Lifecycle

Expected files for the test schema:

```text
ddl.mylite/
  datadir/
    app/
      db.opt
      notes_archive.frm
      notes_archive.MYD
      notes_archive.MYI
```

After `RENAME TABLE`, the old `notes.*` files must be absent and the renamed
files must be present. After `DROP TABLE`, the renamed table files must be
absent. After `DROP DATABASE`, the schema directory must be absent. The
configured external `temp_directory` remains empty for durable database paths.

## Embedded Lifecycle And API

The test uses the existing `mylite_open()`, `mylite_exec()`, and
`mylite_close()` lifecycle. It does not add new API behavior.

## Build, Size, And Dependencies

No new dependencies or embedded profile changes are expected. Binary size should
remain effectively unchanged.

## Test Plan

1. Add `libmylite.embedded-ddl-lifecycle`.
2. Create a schema and controlled MyISAM table, insert one row, alter the table,
   rename it, and close the database.
3. Assert `db.opt`, `.frm`, `.MYD`, and `.MYI` files are inside
   `<db>.mylite/datadir/app/`.
4. Assert old table files are gone after rename.
5. Reopen without `MYLITE_OPEN_CREATE` and read the altered row.
6. Drop the table and database, then assert table files and schema directory are
   removed.
7. Run embedded and non-embedded build/test presets, format check, tidy, diff
   check, and size measurement.

## Acceptance Criteria

- Controlled MyISAM DDL metadata and engine files stay inside the MyLite
  database directory.
- Rename and drop lifecycle assertions pass for `.frm`, `.MYD`, `.MYI`, and
  `db.opt` paths.
- Reopen sees the altered table and row through MariaDB SQL.
- Documentation and compatibility tables describe the covered lifecycle and
  remaining limits.

## Risks And Open Questions

- `.frm` and `db.opt` are MariaDB native metadata files. They are acceptable
  inside `datadir/`, but they are not a final custom MyLite catalog.
- This slice does not prove crash safety during DDL. DDL log and recovery
  behavior belongs to the transactions and recovery slice.
- This slice does not prove lifecycle support for InnoDB, Aria, views,
  triggers, routines, or partition metadata.
