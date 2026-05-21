# Active Index Root Entry Cache

## Problem

Published index-leaf reads now reuse scoped file and header views, but each
leaf-root path still resolves the index-root catalog record by scanning the
current catalog image. Active statements already cache the table catalog entry;
the same repeated statement can still resolve the same index root several times
across exact row-id, exact entryset, full entryset, and FK prefix probes.

The next low-risk performance step is to cache the resolved index-root catalog
entry for the active statement view. This avoids repeated catalog-record scans
while leaving the current published-root, maintained-root, and append-history
fallback semantics unchanged.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- `mylite_storage_read_index_entries()`,
  `mylite_storage_read_exact_index_entries()`, and
  `mylite_storage_index_prefix_exists_for_index()` now use
  `mylite_storage_file_scope` and active table-entry cache lookup before
  calling leaf-root helpers.
- `read_index_leaf_entries()`, `read_index_leaf_exact_entries()`,
  `read_index_leaf_exact_row_ids()`,
  `find_index_leaf_exact_static_row_id()`, and
  `find_static_index_leaf_prefix_exists()` each call
  `find_index_root_record()` over the catalog image for the same
  schema/table/table-id/index tuple.
- `clear_catalog_root_cache()` already invalidates active catalog-image and
  table-entry caches when the catalog root page or generation changes.

## Scope

- Add one active statement cache for the last resolved index-root catalog
  entry.
- Key the cache by schema name, table name, table id, index number, catalog
  root page, and catalog generation.
- Use the cache in published leaf-root readers before scanning the catalog
  image.
- Clear the cache with other catalog-root-derived active statement caches.

## Non-Goals

- No SQL behavior change.
- No file-format change.
- No public API change.
- No B-tree navigation, root split, compaction, pager, or WAL work.
- No durable cross-statement cache in this slice.

## Design

Add `mylite_storage_index_root_entry_cache` beside the existing
`mylite_storage_table_entry_cache` in `mylite_storage_statement`.

The cache stores a copy of the root `mylite_storage_catalog_entry` without its
catalog-image record pointer. A lookup is valid only when all identity fields
and the header catalog fingerprint match. On miss, the existing
`find_index_root_record()` scan runs and the successful result is cached for
later helpers in the same active statement/read statement/snapshot view.

Because the cached entry contains only stable scalar metadata, callers still
validate and decode the actual root page through `read_index_leaf_run_root()`
before using it.

## Compatibility Impact

SQL-visible behavior is unchanged. The cache only reuses catalog metadata that
was already found in the validated catalog image for the same catalog root page
and generation.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. The cache is transient
statement memory and is cleared with the statement or when catalog-root-derived
caches are invalidated.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite index readers benefit. Volatile MEMORY/HEAP row storage paths
are unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to a small cache
struct and helper functions.

## Test Plan

- Re-run storage unit coverage for maintained roots, leaf runs, exact entrysets,
  FK prefix probes, rollback, recovery, and read-statement caches.
- Re-run storage-engine smoke because handler index cursors use these helpers.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- Published leaf-root readers check the active index-root cache before scanning
  the catalog image.
- Catalog invalidation clears the index-root cache with table-entry and
  catalog-image caches.
- Existing storage and storage-smoke tests pass.

## Risks And Open Questions

- This does not avoid the first catalog-image copy for a statement. It removes
  repeated root-record scans after one successful resolution; avoiding the first
  image copy remains broader catalog/root lookup work.

## Verification Results

Recorded 2026-05-21:

- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  reported no changes after formatting adjustment.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed.
- `git diff --check` passed.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  libmylite.embedded-storage-engine --output-on-failure` passed.
