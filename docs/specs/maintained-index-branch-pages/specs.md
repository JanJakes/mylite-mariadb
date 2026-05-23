# Maintained Index Branch Pages

## Problem

Published leaf runs give MyLite exact fixed-width byte-key lookup over rebuilt
snapshots, and single-page maintained roots cover small indexes with in-place
DML. They do not yet provide a navigable multi-page maintained index. The next
storage step needs an internal branch page format that can map a probe key to a
child page without scanning every leaf page.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c` and
  `packages/mylite-storage/src/storage_format.h`; no upstream MariaDB source
  files need to change.
- Current maintained root and published leaf-run readers already validate page
  ownership, fixed raw key width, sorted `(key, row_id)` order, checksums, and
  child row/page addressability before trusting page contents.

## Scope

- Add a fixed-width `TABLE_INDEX_BRANCH` page type for first-party MyLite
  storage.
- Store branch cells as child page id plus high `(key, row_id)` fence key.
- Add encode/decode helpers with checksum, format-version, page-id,
  table/index, level, child-count, used-byte, child-page, and sorted-fence
  validation.
- Add a binary-search helper that returns the first child whose fence is greater
  than or equal to the probe `(key, row_id)`.
- Treat branch pages as known typed pages in existing non-branch decoders and
  recovery journal validation.

## Non-Goals

- No production B-tree root publication yet.
- No split, merge, rebalance, or in-place branch-page DML.
- No public storage API or SQL behavior change.
- No variable-width or collation-aware branch keys; this slice follows the
  current fixed raw byte-key index format.

## Compatibility Impact

No SQL-visible behavior changes. The branch page is a storage-internal
foundation for future MySQL/MariaDB-compatible index access paths. `ENGINE`
routing, DDL metadata, and public `libmylite` behavior remain unchanged.

## Single-File And Lifecycle Impact

Branch pages are durable pages inside the primary `.mylite` file. The slice
does not introduce any new companion files. Recovery journal validation learns
the new page type so future active dirty-page protection can validate saved
branch pages before replay.

## File-Format Impact

Adds page type `TABLE_INDEX_BRANCH` (`13`) with page-local version `1`. The
global storage format version is not bumped because the page type is not yet
written by production paths and is guarded by explicit type/version validation.

## Test Plan

- Add storage test hooks for branch-page encode, decode, and child search.
- Verify canonical encode/decode, cell layout, boundary child lookup, and
  beyond-last lookup.
- Verify corrupt page id, table id, level, key size, used bytes, child page id,
  fence ordering, duplicate fence row id, and checksum failures.
- Verify misuse and capacity errors from the encoder and search helper.
- Run:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/src/storage_format.h packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure
```

## Acceptance Criteria

- Branch pages round-trip through encode/decode under test hooks.
- Decode rejects malformed or unsafe branch pages before exposing payload
  pointers.
- Binary child lookup follows the branch fence ordering.
- Existing storage and routed storage-engine smoke tests keep passing.
- Roadmap documentation continues to show production B-tree navigation as
  pending rather than implied complete.

## Risks And Follow-Ups

- The page format uses fixed raw keys only. Future collation-aware or
  variable-width keys need a separate design.
- Parent/root publication, split/merge policy, dirty-page protection, and
  transactional branch maintenance remain separate slices.
- A future format bump may still be needed once production branch pages are
  written by stable releases.

## Implementation Notes

The slice adds `TABLE_INDEX_BRANCH` page type `13`, private encode/decode
helpers, binary child search, storage test hooks, and branch-page awareness in
typed-page skip/validation paths. Production index reads still use maintained
roots, published leaf runs, and append-tail overlays.

## Verification Results

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/src/storage_format.h packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
