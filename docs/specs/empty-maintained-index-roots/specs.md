# Empty Maintained Index Roots

## Problem

Fresh indexed MyLite tables append fallback index-entry pages until a later
copy-rebuild DDL publishes an index root. A table with a primary key and one
raw integer secondary key therefore starts rootless, writes more pages per
initial insert, and cannot use maintained-root point lookup behavior until a
later rebuild.

Fresh table creation should publish empty mutable roots for the raw fixed-width
key shapes already supported by maintained-root DML. Once fresh roots exist,
row replacement must keep those roots pointing at the current row page even
when MariaDB reports only the keys that changed.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::create()` stores the MyLite
  table-definition catalog record, then foreign-key metadata and
  autoincrement state. It did not publish index roots for keys present in the
  new table definition.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_rebuild_index_leaf_roots()`
  already selects supported raw fixed-width integer-family keys after
  copy-rebuild DDL and calls `mylite_storage_rebuild_index_leaves()`.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_key_is_supported()` rejects
  unsupported index classes, and
  `mylite_key_uses_raw_exact_filter()` limits raw storage roots to
  integer-like fixed-width fields.
- `packages/mylite-storage/src/storage.c::mylite_storage_append_row_with_index_entries()`
  plans maintained-root inserts when the catalog has an index-root record for
  the table; otherwise it writes fallback index-entry pages.
- `packages/mylite-storage/src/storage.c::encode_maintained_index_root_page_from_entryset()`
  can encode an empty maintained root when the caller supplies a non-zero key
  size.
- Direct UPDATE can pass only keys that may change. With fresh roots, an UPDATE
  of an unsupported string index can replace the row while omitting the
  unchanged primary-key entry from the handler call, so storage must retarget
  maintained roots that still reference the source row and were not already
  updated by changed-entry planning.

## Scope

- Add a storage API that creates empty maintained index-root pages and catalog
  root records for an existing table.
- Use that API during non-temporary file-backed `CREATE TABLE` for supported
  raw fixed-width keys, including requests routed from `ENGINE=InnoDB`.
- Keep BLACKHOLE row-discard tables, MEMORY/HEAP volatile-row tables,
  temporary tables, and unsupported key shapes out of this publication path.
- Keep maintained roots correct when row replacement preserves all index keys,
  or changes only keys that do not have maintained roots.
- Reset maintained roots during `TRUNCATE` so stale root entries do not survive
  table-emptying DDL.
- Preserve transaction and read-statement snapshots for all protected pages
  needed by in-place maintained-root updates.

## Non-Goals

- No multi-row row-page packing. Row ids still map to row page ids.
- No root publication for string, BLOB/TEXT prefix, nullable-specialized,
  FULLTEXT, SPATIAL, vector, or hash index shapes beyond the existing raw
  fixed-width path.
- No branch-root retarget support for unchanged keys in this slice; branch
  update support remains limited to keys represented in the changed-entry plan.
  Omitted branch roots and overflow-tail roots return unsupported instead of
  silently leaving stale row ids.
- No new public `libmylite` API.
- No SQL-visible indexing semantic change.

## Design

The storage layer exposes a MyLite-owned internal API:

```c
mylite_storage_initialize_empty_index_roots(
    filename, schema_name, table_name, definitions, definition_count
)
```

Each definition names an index number and fixed key size. The API validates the
table record, rejects duplicate definitions or already-existing roots, appends
one empty maintained-root page per definition at the current file end, appends
matching catalog root records with `entry_count = 0`, advances `page_count`,
and publishes the catalog/header through the normal journaled catalog update
path.

The handler calls this API from `ha_mylite::create()` immediately after storing
the table definition and before ordinary row DML can run. It selects the same
supported raw exact key family used by the copy-rebuild root path and passes
`KEY::key_length` as the root key size.

For row replacement, storage now plans maintained-root retargets for any
maintained root that still points at the source row and was not already handled
by the changed-entry update plan. This covers direct UPDATE shapes where
MariaDB passes only a changed unsupported key, while the unchanged primary-key
root still needs its row id moved to the replacement row page.

`TRUNCATE` collects maintained roots for the table and rewrites them as empty
roots as part of the truncate publication. Transaction journal snapshots and
read-statement transaction snapshots retain all protected pages needed to keep
root updates and rollbacks visible through the correct statement view.

## Compatibility Impact

SQL behavior should not change. Supported raw integer primary and secondary
indexes are still maintained by MyLite storage and remain visible through the
same handler reads. The storage layout changes only the starting point: fresh
tables begin with empty maintained roots instead of rootless append-tail index
pages.

The engine-routing compatibility contract is preserved. A file-backed
`ENGINE=InnoDB` table routes to MyLite and receives MyLite roots for eligible
keys; native InnoDB files are still not created.

## DDL Metadata Routing Impact

The table-definition catalog record remains authoritative for MariaDB table
metadata. The new root records are auxiliary MyLite storage metadata under the
same schema/table name and follow existing drop, rename, schema-drop, and
truncate catalog behavior.

## Single-File And Lifecycle Impact

All new durable state is written inside the primary `.mylite` file. The normal
rollback journal protects catalog/header publication and maintained-root
updates. No new persistent or transient sidecar is introduced.

## File-Format And API Impact

No new page type is introduced. Empty roots reuse the existing maintained
`TABLE_INDEX_ROOT` page format. The storage header remains at the same format
version. The new API is in the internal `mylite-storage` surface used by the
handler, not in the public `libmylite` C API.

## Binary-Size Impact

The change adds first-party storage helpers and one handler selection helper.
It introduces no dependency and should not affect the embedded profile's
external component set.

## Test Plan

- Add storage unit coverage for empty root initialization:
  - invalid and duplicate definition handling,
  - initial root metadata and page type,
  - first row append advancing `page_count` by one row page rather than row
    plus fallback index-entry pages,
  - exact lookup through initialized roots,
  - preserved-key row replacement retargeting maintained roots,
  - combined changed-root update and unchanged-root retarget rollback,
  - changed unsupported-key row replacement retargeting unchanged maintained
    roots,
  - truncate resetting stale maintained roots.
- Add embedded storage-engine coverage for a fresh routed `ENGINE=InnoDB`
  table with a primary key and integer secondary key.
- Update copy-ALTER root publication coverage so raw integer roots are expected
  before ALTER, while string keys still have no initial root.
- Run focused storage, embedded, static, and performance checks.

## Verification Results

Local environment: macOS worktree, `dev` and `storage-smoke-dev` presets.

- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `153.30 sec`.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  libmylite.embedded-storage-engine --output-on-failure`: passed in
  `33.09 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`:
  passed.
- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/include/mylite/storage.h
  packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
  mariadb/storage/mylite/ha_mylite.cc
  packages/libmylite/tests/embedded_storage_engine_test.c`: passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-select-components 1000 10000` reported the prepared
  primary-key row component at `0.482 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 1000` reported the prepared insert
  step component at `160.537 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 10000` reported the prepared insert
  step component at `1450.113 us/op` and commit at `1031.048 ms`.

## Acceptance Criteria

- Fresh durable MyLite tables publish empty maintained roots for supported raw
  fixed-width keys.
- Initial inserts into those tables do not append fallback index-entry pages
  for roots that can be maintained in place.
- Unsupported key shapes remain on the existing append-tail or rebuild path.
- Root metadata survives rename, copy-ALTER, reopen, and truncate through
  existing catalog behavior.
- Preserved-key and partial key-changing row replacements retarget maintained
  roots to the replacement row id.
- Transaction reads and rollbacks see protected maintained-root pages through
  the correct snapshot.

## Risks And Open Questions

- Maintained-root inserts initially regressed the prepared insert component
  benchmark because the root-maintenance path rewrote protected root pages as
  rows were inserted. The follow-up dirty-page buffer removes repeated
  same-root writes from active checkpoints and materially lowers deferred
  commit publication cost, but row-page layout and branch navigation still
  dominate larger insert loops.
- This narrows write volume for indexed inserts but does not address one row
  page per row. Row packing remains a larger file-format and row-id design
  slice.
- Branch-root retargeting for unchanged roots is not complete. The current
  retarget path covers single-page maintained roots; branch roots and
  overflow-tail roots still need a dedicated preserved-key retarget slice.
- Nullable raw integer keys are not separated in this slice; they follow the
  same raw-exact key selection already used by copy-rebuild root publication.
  Broader nullable and collation-sensitive key navigation remains separate
  roadmap work.
