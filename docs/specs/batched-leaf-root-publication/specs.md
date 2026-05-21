# Batched Leaf-Root Publication

## Problem

MyLite now publishes catalog-backed fixed-width index leaf roots after
copy-style SQL rebuilds for every current raw key in the rebuilt table. The
handler still calls the storage rebuild API once per key. Each call opens the
primary file, reads the header and catalog, scans the whole append history,
writes one leaf run, and publishes one catalog image.

That makes multi-key copy rebuilds pay append-history scan cost once per raw
key even though all rebuilt keys share the same table id and row-state history.
It also publishes several catalog generations where one atomic publication
would describe the same rebuilt table snapshot.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_table.cc::mysql_alter_table()` sets `ALTER_RECREATE` for
  same-engine `ALTER TABLE ... ENGINE=...` and `ALTER TABLE ... FORCE`, then
  uses the copy path when the requested algorithm or table shape requires it.
- MariaDB finalizes copy ALTER by renaming the old table to a backup name and
  renaming the rebuilt temporary table to the target name. MyLite observes that
  final rename in `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::rename_table()`.
- `ha_mylite::rename_table()` currently calls
  `mylite_rebuild_index_leaf_roots()` after the final copy-style rename. That
  helper iterates `TABLE_SHARE::keys`, drops roots for supported non-raw keys,
  and calls `mylite_storage_rebuild_index_leaf()` once for each supported raw
  key.
- `packages/mylite-storage/src/storage.c::mylite_storage_rebuild_index_leaf()`
  rebuilds one leaf root. It reads live entries through
  `read_live_index_entries()`, which scans every page from the empty-file page
  boundary and applies row-state replacement/delete pages to one entryset.
- The row-state overlay rules are table-wide: a row replacement retargets live
  entry row ids for every index whose key image did not change, and a delete or
  truncate removes the source row from every live index entryset.

## Design

Add a storage batch rebuild operation for a set of index numbers on one table:

- validate the table once and reject empty or duplicate index-number lists as
  misuse;
- open the primary file, read the header, read the catalog, and locate the
  table id once;
- scan append history once into one live entryset per requested index number;
- apply each row-state replacement, delete, or truncate operation to every
  requested entryset while sharing one row-state visibility map;
- prepare a sorted contiguous leaf run for every requested index number;
- remove stale root records for every requested index number from one catalog
  image and append replacement root records with the correct page ranges and
  entry counts;
- write all leaf pages in one contiguous append range; and
- publish one updated catalog/header image under one rollback journal.

Keep `mylite_storage_rebuild_index_leaf()` as a wrapper over the batch API so
existing storage tests and narrow callers retain the same behavior. The handler
should collect all current supported raw key numbers and call the batch API
once, while still dropping roots for supported non-raw keys individually so key
shape changes cannot leave stale fixed-width roots behind.

## Non-Goals

- No new file-format fields.
- No B-tree navigation or maintained page splits.
- No row-DML leaf-root maintenance.
- No variable-width or collation-aware leaf encoding.
- No change to SQL-visible copy ALTER semantics.

## Compatibility Impact

SQL-visible behavior should not change. The batched rebuild publishes the same
immutable base leaf runs as repeated single-index rebuilds, but with one
append-history scan and one catalog publication. MariaDB still owns table-copy
execution, row production, key tuple generation, diagnostics, and result
ordering.

## Single-File And Lifecycle Impact

All new leaf pages and root records stay inside the primary `.mylite` file. No
new companions are introduced. The existing storage rollback journal protects
the combined append and catalog publication, and the surrounding SQL statement
checkpoint still owns copy ALTER rollback.

## Public API, File-Format, And Dependency Impact

The internal first-party storage header gains
`mylite_storage_rebuild_index_leaves()` for batch publication. `libmylite` has
no public API change, the file-format version does not change, and no new
dependency is added.

## Tests

- Add storage-unit coverage for a table with two fixed-width indexes where the
  batch API publishes both roots in one call and exact/full index reads remain
  correct after appends, unchanged-key updates, changed-key updates, and
  deletes.
- Keep existing single-index rebuild tests passing through the wrapper API.
- Keep embedded copy ALTER leaf-root publication coverage passing with the
  handler using one batch rebuild for raw keys.

Run at least:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build build/mariadb-mylite-storage-smoke --target libmariadbd.a
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
git diff --check
```

## Acceptance Criteria

- SQL copy-rebuild leaf-root publication rebuilds all current raw fixed-width
  key roots with one storage batch call.
- The storage batch operation scans live append history once for all requested
  indexes and publishes one catalog image.
- The existing single-index rebuild API remains source-compatible and keeps its
  behavior.
- Root metadata and exact/full reads match the existing single-index rebuild
  behavior after append-tail insert, update, and delete overlays.
- Storage and embedded storage-engine tests pass.
