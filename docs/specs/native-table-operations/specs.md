# Native Table Operations

## Goal

Validate a broader set of native MyISAM table operations through `libmylite`:
table scans, indexed lookups, row insert/update/delete, unique-key failures,
nullable unique-key behavior, autoincrement persistence, BLOB/TEXT values, and
copy-style `ALTER TABLE` rebuilds inside one MyLite database directory.

## Non-Goals

- Do not enable or claim InnoDB or Aria table lifecycle support.
- Do not implement transaction, rollback, crash-recovery, locking, or
  concurrency guarantees.
- Do not add a custom storage engine or MyLite metadata catalog.
- Do not broaden the public API beyond `mylite_open()`, `mylite_exec()`, and
  `mylite_close()`.
- Do not implement binary-safe result APIs; this slice can verify BLOB/TEXT
  values through SQL expressions and textual result callbacks.
- Do not perform size profile hardening.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_insert.cc:2031` routes writes to
  `table->file->ha_write_row()`. Duplicate-key handling can locate the
  offending row through `ha_rnd_pos()` or `ha_index_read_idx_map()` at
  `mariadb/sql/sql_insert.cc:2034-2100`.
- `mariadb/sql/sql_update.cc:1059-1064` routes ordinary updates through
  `table->file->ha_update_row()`.
- `mariadb/sql/sql_delete.cc:952-963` routes ordinary deletes through
  `TABLE::delete_row()`, which reaches the selected handler.
- `mariadb/sql/handler.cc:3796-3839` wraps full table scans through
  `handler::ha_rnd_next()`.
- `mariadb/sql/handler.cc:3890-3942` wraps key lookups through
  `handler::ha_index_read_map()` and `handler::ha_index_read_idx_map()`.
- `mariadb/storage/myisam/ha_myisam.cc:338-397` maps BLOB/TEXT fields into
  MyISAM record descriptors.
- `mariadb/storage/myisam/ha_myisam.cc:952-965` implements MyISAM writes and
  updates autoincrement values before `mi_write()`.
- `mariadb/storage/myisam/ha_myisam.cc:1937-1944` implements MyISAM row update
  and delete through `mi_update()` and `mi_delete()`.
- `mariadb/storage/myisam/ha_myisam.cc:1983-2005` maps MyISAM index reads to
  `mi_rkey()`, and `mariadb/storage/myisam/ha_myisam.cc:2050-2060` maps table
  scans to `mi_scan()`.
- `mariadb/storage/myisam/ha_myisam.cc:2335-2351` reports MyISAM
  autoincrement state from table metadata.
- `mariadb/storage/myisam/ha_myisam.cc:2439-2540` decides which ALTER TABLE
  changes can remain compatible and when copy/rebuild behavior is required.

The existing MyLite runtime already points MariaDB's data directory at
`<db>.mylite/datadir`, so this slice is expected to add test coverage and
documentation rather than new storage routing code.

## Compatibility Impact

This slice moves the MyISAM row/index surface from planned to partial coverage.
It demonstrates that common row operations and index semantics use MariaDB's
native MyISAM handler while staying inside the MyLite directory. The coverage is
still bounded: it does not prove all data types, all index algorithms,
transactional behavior, concurrent behavior, or non-MyISAM engines.

## Design

Add an embedded integration test that creates one durable `.mylite/` directory,
creates a MyISAM table with:

- an autoincrement primary key,
- a nullable unique key,
- a secondary key,
- `TEXT` and `BLOB` columns,
- later copy-style `ALTER TABLE` changes.

The test should exercise row insert/update/delete, duplicate-key errors, table
scans, indexed predicates, BLOB/TEXT length checks, copy `ALTER TABLE`, close
and reopen, and post-reopen autoincrement state. File assertions should confirm
the MyISAM `.frm`, `.MYD`, and `.MYI` files remain under
`<db>.mylite/datadir/<schema>/`.

## File Lifecycle

Expected table files while the table exists:

```text
native-ops.mylite/
  datadir/
    app/
      native_ops.frm
      native_ops.MYD
      native_ops.MYI
```

The configured external runtime root remains transient and empty for durable
database paths. Clean close removes `run/` and leaves durable native table files
under `datadir/`.

## Embedded Lifecycle And API

The slice uses the existing public `libmylite` lifecycle. It may assert:

- `mylite_last_insert_id()` after autoincrement inserts,
- `mylite_changes()` after updates and deletes,
- duplicate-key diagnostics exposed by `mylite_errcode()`,
  `mylite_mariadb_errno()`, `mylite_sqlstate()`, and `mylite_errmsg()`.

## Build, Size, And Dependencies

No new runtime dependencies or embedded profile changes are expected. Binary
size should remain effectively unchanged because this slice adds tests and docs
only.

## Test Plan

1. Add `libmylite.embedded-native-table-operations`.
2. Create a schema and MyISAM table with autoincrement, primary, unique,
   secondary, `TEXT`, and `BLOB` columns.
3. Insert rows with non-null and null unique-key values; assert insert ids.
4. Assert duplicate non-null unique-key inserts fail with MariaDB duplicate-key
   diagnostics.
5. Update one row, delete another row, and assert affected-row counts.
6. Query by secondary key, unique key, and full table scan with deterministic
   ordering.
7. Assert large `TEXT` and `BLOB` values survive update and SQL reads.
8. Force a copy-style `ALTER TABLE` that adds a column and secondary index,
   then assert existing rows retain default values and indexed reads work.
9. Close and reopen without `MYLITE_OPEN_CREATE`, insert another row, and
   assert autoincrement state persists.
10. Assert MyISAM table files stay inside the MyLite database directory and no
    durable files appear in the external runtime root.
11. Run embedded and non-embedded build/test presets, format check, tidy, diff
    check, and size measurement.

## Acceptance Criteria

- The new test passes against the embedded MariaDB-backed profile.
- Insert, update, delete, scans, indexed reads, unique-key errors,
  nullable unique keys, autoincrement, and BLOB/TEXT checks are covered for a
  controlled MyISAM table.
- Copy-style `ALTER TABLE` preserves existing rows and native table files inside
  `datadir/`.
- Documentation and compatibility tables describe the covered row/index surface
  and remaining limits.

## Risks And Open Questions

- MyISAM is not transactional; this slice is not recovery or rollback evidence.
- The direct SQL callback still exposes textual result values only. Binary-safe
  values need the prepared SQL API slice.
- Optimizer choices are not explicitly asserted. SQL predicates over indexed
  columns exercise the handler surface, but detailed plan validation belongs to
  compatibility harness work.
- Engine-specific behavior for InnoDB and Aria remains planned.
