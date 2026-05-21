# Copy ALTER Leaf-Root Publication

## Problem

MyLite can publish catalog-backed fixed-width index leaf runs and use them for
exact and full index reads. SQL currently publishes those roots only for
explicit fixed-width `CREATE INDEX` and `ALTER TABLE ... ADD KEY` paths. A table
rebuilt by MariaDB's copy ALTER path still leaves retained raw keys, including
the common primary-key shape created with the table, on the append-history scan
path.

That makes post-load point reads depend on append-only exact-index scans unless
the application happens to add a new fixed-width index after loading data.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_table.cc::mysql_alter_table()` marks same-engine
  `ALTER TABLE ... ENGINE=...` and `ALTER TABLE ... FORCE` with
  `ALTER_RECREATE`, then uses the copy ALTER path when in-place execution is
  impossible or copy is requested.
- `mariadb/sql/sql_table.cc` finalizes copy ALTER by renaming the old table to a
  backup name and renaming the new temporary table to the target name. MyLite's
  `ha_mylite::rename_table()` sees that final rename after rows and index-entry
  pages have been written into the rebuilt table.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::rename_table()` currently
  calls `mylite_rebuild_index_leaf_roots()` only when
  `mylite_current_command_publishes_index_leaf_roots()` sees
  `SQLCOM_CREATE_INDEX` or `ALTER_ADD_INDEX`.
- `packages/mylite-storage/src/storage.c::mylite_storage_rebuild_index_leaf()`
  rebuilds one catalog-backed leaf run from the current live index entries and
  replaces any existing root record for that table/index number.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_key_uses_raw_exact_filter()`
  identifies fixed-width integer/year key parts whose stored key bytes can be
  used by the current raw leaf-run format.

## Design

After a successful final copy-style table rename, rebuild leaf roots for every
current supported raw fixed-width key in the rebuilt table:

- keep the backup-table rename excluded so failed ALTER rollback can restore the
  original table state;
- skip volatile and row-discarding requested engines;
- read the rebuilt table definition through the existing catalog table-share
  helper;
- for each supported current key, call `mylite_storage_rebuild_index_leaf()`
  when the key fits the raw fixed-width leaf format;
- drop any current non-raw root record for the same index number so a key shape
  change cannot leave stale leaf metadata behind;
- treat storage-full, unsupported, and not-found rebuild results as non-fatal
  opportunistic publication misses, matching the current explicit-index policy.

The SQL command filter should cover copy DDL commands that rebuild table rows:
standalone `CREATE INDEX`, standalone `DROP INDEX`, and `ALTER TABLE` commands
other than pure rename-only ALTERs. The final `rename_table()` call remains the
practical gate that the copy path actually ran.

## Non-Goals

- No maintained B-tree pages or page splits.
- No automatic leaf-root rebuild after ordinary DML or transaction commit.
- No new file-format fields.
- No variable-width or collation-aware leaf-run encoding.
- No support for online or in-place ALTER paths.

## Compatibility Impact

SQL-visible behavior should not change. Leaf roots are read-side acceleration
metadata over the same live index-entry and row-state history. MariaDB remains
responsible for SQL parsing, copy ALTER row production, key tuple generation,
and result ordering.

The change can make fixed-width retained keys, including primary keys and
foreign-key helper keys that are present in the rebuilt table definition, use the
published-root path after a copy rebuild.

## Single-File And Lifecycle Impact

Published leaf pages and catalog root records remain inside the primary
`.mylite` file. The slice adds no companion files and no new recovery surfaces.
The existing statement checkpoint around copy ALTER owns rollback if publication
fails before statement commit.

## Public API, File-Format, And Dependency Impact

No public API change, no file-format version change, and no new dependency.

## Tests

- Add embedded storage-engine coverage where a table has a primary key, a raw
  secondary key, and a non-raw secondary key, then `ALTER TABLE ... FORCE,
  ALGORITHM=COPY` publishes roots for the raw retained keys but not the non-raw
  key.
- Verify point reads still work after the forced rebuild and after rows are
  inserted, updated, and deleted after the root publication.
- Update storage architecture, compatibility, and roadmap text to describe
  copy-rebuild retained-key publication.

Run at least:

```sh
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
```

## Acceptance Criteria

- Copy-style MyLite table rebuilds publish leaf roots for all current supported
  raw fixed-width keys in the rebuilt table.
- Pure table renames do not perform opportunistic leaf-root rebuilds.
- Non-raw current keys do not retain stale leaf roots.
- Existing explicit fixed-width `CREATE INDEX` and `ALTER TABLE ... ADD KEY`
  publication behavior remains covered.
- Relevant storage and embedded storage-engine tests pass.
