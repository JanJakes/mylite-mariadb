# Index Leaf Run Page Search

## Problem

Contiguous index leaf runs let fixed-width SQL indexes publish more than one
leaf page, but exact lookup still reads every page in the run before applying
the append-tail overlay. That removes the old single-page publication limit,
yet point reads remain linear in published leaf pages.

## Design

- Keep the current leaf page format and catalog root metadata.
- Derive the contiguous run length from root `entry_count`, the root leaf
  `key_size`, and the leaf entry capacity.
- Validate each visited run page against the owning table id, index number,
  root key size, and expected page entry count. Missing pages inside a published
  run remain corruption, not fallback.
- For exact lookup, binary-search leaf page key ranges instead of walking the
  full run.
- For non-unique keys that can span page boundaries, walk backward while the
  previous page's last key matches, then scan forward while following pages
  begin with the requested key.
- Continue scanning only pages appended after the published run as the
  authoritative row-state and index-entry overlay for later mutations.

## Compatibility Impact

No SQL-visible behavior changes. Raw byte-key equality, duplicate ordering,
append-tail visibility, and row-state pruning are preserved.

## Single-File And Lifecycle Impact

No new files or lifecycle states. The optimization only changes how published
leaf pages already stored in the primary `.mylite` file are read.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

Routed MyLite exact reads over opportunistically published fixed-width SQL index
roots can use the page-range path. Missing roots and unsupported index shapes
continue to use the append-only scan fallback.

## Tests And Verification

- Add storage unit coverage for a duplicate key spanning adjacent leaf pages.
- Verify first-page, later-page, last-page, missing-key, and append-tail lookups
  still work over a multi-page run.
- Run the storage unit test, storage-engine smoke harness, compatibility
  harness, performance baseline, formatting check, and whitespace check.

## Local Verification

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 1`
  - Published `perf_leaf_rows` index `1` with `1000` entries.
  - Direct published-leaf secondary exact read: `680554 us/op`.
  - Prepared published-leaf secondary exact read: `662317 us/op`.
- `/opt/homebrew/opt/llvm/bin/git-clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `git diff --check`

## Acceptance Criteria

- Exact lookup over a published run no longer reads every leaf page for ordinary
  point reads.
- Duplicate keys spanning page boundaries return all matching row ids.
- Append-tail index entries and row-state pages remain visible after a published
  run.

## Risks

- This is still a rebuilt static leaf run, not a maintained B-tree. Inserts,
  updates, and deletes after publication continue to use the append-tail overlay.
- Full cross-page ordering validation would require reading the whole run. The
  fast path validates visited pages and relies on the rebuild writer to have
  produced globally sorted contiguous pages.
