# Allocation-Free Prefix Existence Overlay

## Goal

Answer durable index-prefix existence fallback checks without materializing
matching key images. The fallback should track only matching row ids while
preserving the same append-tail row-state overlay semantics used by prefix
entryset reads.

## Non-Goals

- Do not change SQL-visible duplicate-key, foreign-key, or prefix-index
  behavior.
- Do not change the static no-tail allocation-free fast path.
- Do not add B-tree mutation, compaction, or a new file format.
- Do not change durable prefix entryset reads used by grouped autoincrement.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `packages/mylite-storage/src/storage.c:mylite_storage_index_prefix_exists_for_index()`
  now falls back to `read_index_leaf_prefix_entries()` or
  `scan_index_prefix_entries_from()` when static no-tail leaf roots cannot
  answer directly.
- `scan_index_prefix_entries_from()` preserves visibility by pretracking base
  row ids, removing or replacing tracked row ids as later row-state pages are
  decoded, and appending matching tail entries.
- Prefix existence does not need key images after prefix filtering; it only
  needs to know whether any live matching row id remains after applying
  `skip_row_id`.
- `mylite_storage_live_row_id_set` already provides add, remove, contains, and
  cleanup helpers suitable for row-id-only tracking.

## Compatibility Impact

SQL-visible behavior should not change. The row-id overlay follows the same
visibility rules as the prefix entryset fallback it replaces.

## Affected Subsystems

- Durable storage prefix-existence API.
- Foreign-key and prefix-index duplicate checks that call
  `mylite_storage_index_prefix_exists_for_index()`.
- Storage unit tests and architecture documentation.

## Design

Add an internal `scan_index_prefix_exists_from()` helper that:

1. starts with a row-id set containing matching static leaf entries when a leaf
   root exists;
2. scans append-tail index-entry and row-state pages from the requested page id;
3. removes stale matching row ids before adding newer matching tail entries;
4. mirrors replacement/delete row-state handling with row-id set operations;
5. returns true only when a matching row id other than `skip_row_id` remains.

When no leaf root exists, the helper starts with an empty row-id set and scans
from the append-history start. Static no-tail leaf roots keep
`scan_static_index_leaf_prefix_exists()` because that path can return as soon
as it sees a matching non-skipped row.

## File Lifecycle

No file-format or companion-file change.

## DDL Metadata Routing Impact

No metadata change. Existing table and index-root catalog records are reused.

## Embedded Lifecycle And API

No public API change. This is an internal storage implementation detail.

## Storage-Engine Routing

MyLite-routed durable tables benefit through existing prefix-existence callers.
Runtime-volatile MEMORY/HEAP behavior is unchanged.

## Build, Size, And Dependencies

No dependency or intended size-profile change. The slice should reduce
temporary heap pressure on prefix-existence fallback paths by avoiding key
image arrays.

## Test Plan

- Keep storage coverage for static roots, append-tail inserts, missing
  prefixes, changed-key updates, deletes, and `skip_row_id` passing.
- Run storage unit tests, storage-smoke, clang-format diff, and `git diff
  --check`.

## Acceptance Criteria

- `mylite_storage_index_prefix_exists_for_index()` no longer builds a
  `mylite_storage_index_entryset` in its append-tail or no-root fallback.
- `skip_row_id` behavior is preserved after append-tail inserts, updates, and
  deletes.
- Existing storage and embedded tests pass.

## Verification Results

2026-05-23, macOS arm64 local worktree:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --preset storage-smoke-dev --output-on-failure
```

All passed.

## Risks And Unresolved Questions

- The helper still scans append-tail pages. B-tree navigation and fuller
  free-space/index maintenance remain separate roadmap work.
