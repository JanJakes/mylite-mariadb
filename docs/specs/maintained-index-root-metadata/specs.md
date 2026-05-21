# Maintained Index Root Metadata

## Problem

Maintained index roots now publish and read through the root page type, but
`mylite_storage_read_index_root()` still reports the catalog `entry_count`.
That is fine immediately after rebuild publication because the catalog count
matches the page count, but it will become stale once row-DML updates maintained
roots in place without republishing catalog metadata.

This slice makes root metadata reads decode maintained root pages and report
the page-owned entry count.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::mylite_storage_read_index_root()`
  currently reads only catalog metadata and returns
  `entry.definition_size`.
- `packages/mylite-storage/src/storage.c::decode_maintained_index_root_page()`
  validates maintained root ownership, key width, used bytes, checksum, sorted
  order, and row-id addressability.
- `packages/mylite-storage/src/storage.c::read_index_leaf_run_root()` already
  treats the maintained root page as authoritative for reader entry count.

## Scope

- When `mylite_storage_read_index_root()` finds a catalog root, read the root
  page.
- If the root page is `TABLE_INDEX_ROOT`, decode it, verify table/index
  ownership, and return the page-owned entry count.
- Keep immutable leaf-run metadata behavior unchanged.

## Non-Goals

- No in-place root DML maintenance.
- No catalog format change.
- No public API shape change.

## Compatibility Impact

No SQL-visible behavior changes. The public storage metadata API becomes
correct for maintained roots whose catalog count may later become stale.

## Test Plan

- Publish a maintained root through a small rebuild.
- Deliberately republish the catalog root record with a stale entry count.
- Verify `mylite_storage_read_index_root()` still returns the maintained root
  page's entry count.
- Verify immutable leaf-run metadata continues to return catalog entry count.
- Run:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
```

## Acceptance Criteria

- Maintained root metadata reads use page-owned entry counts.
- Immutable leaf-run metadata remains unchanged.
- The later in-place root maintenance slice can update root pages without also
  republishing catalog metadata solely for entry counts.

## Initial Implementation

`mylite_storage_read_index_root()` now reads the catalog root page before
returning metadata. Immutable leaf-run roots keep returning catalog
`entry_count`; maintained root pages are decoded and validated, and the API
returns the page-owned entry count. Storage unit coverage republishes a
maintained root record with a deliberately stale catalog count and verifies the
page count wins, then republishes an immutable leaf-run root with a stale count
and verifies leaf metadata remains catalog-owned.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
