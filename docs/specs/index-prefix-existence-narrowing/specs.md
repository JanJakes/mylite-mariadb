# Index Prefix Existence Narrowing

## Goal

Narrow durable index-prefix existence fallback reads so they materialize only
live entries matching the requested serialized key prefix. This builds on
[Index Prefix Entryset Read](../index-prefix-entryset-read/specs.md) and keeps
prefix duplicate checks from falling back to full index entrysets when a
published leaf root has append-tail overlays or when no leaf root is available.

## Non-Goals

- Do not change SQL-visible duplicate-key, prefix-index, or grouped
  autoincrement semantics.
- Do not add B-tree mutation, compaction, or a new file format.
- Do not change the static no-tail prefix-exists fast path.
- Do not add collation-aware storage comparisons; callers still provide
  MariaDB-built serialized key bytes.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/mylite/ha_mylite.cc:mylite_prefix_key_exists()` calls
  `mylite_storage_index_prefix_exists_for_index()` for durable prefix-existence
  checks.
- `packages/mylite-storage/src/storage.c:mylite_storage_index_prefix_exists_for_index()`
  first asks `find_static_index_leaf_prefix_exists()` to answer no-tail leaf
  roots without materializing an entryset.
- When that static fast path cannot answer, the same function currently falls
  back to full `read_index_leaf_entries()` or `read_live_index_entries()` and
  filters the complete entryset with `index_entryset_prefix_exists()`.
- `packages/mylite-storage/src/storage.c:mylite_storage_read_index_prefix_entries()`
  and its internal `read_index_leaf_prefix_entries()` /
  `scan_index_prefix_entries_from()` helpers already preserve append-tail
  row-state overlay semantics while returning only matching prefix entries.

## Compatibility Impact

SQL-visible behavior should not change. The durable prefix-existence result is
still computed from the live row/index state with the same `skip_row_id`
handling used by duplicate checks and prefix-index probes.

## Affected Subsystems

- First-party durable storage prefix-existence API.
- Prefix-index duplicate checks that route through
  `mylite_storage_index_prefix_exists_for_index()`.
- Storage unit tests and architecture documentation.

## Design

Keep the existing no-tail static leaf fast path because it can answer existence
without allocating an entryset.

When that fast path cannot answer:

1. initialize a temporary `mylite_storage_index_entryset`;
2. call `read_index_leaf_prefix_entries()` with the already-open scoped file,
   resolved table metadata, active statement cache, and requested prefix;
3. if no leaf root exists, call `scan_index_prefix_entries_from()` from the
   append-history start;
4. run the existing `index_entryset_prefix_exists()` over the narrowed entryset
   to preserve `skip_row_id` semantics;
5. free the temporary entryset before returning.

This avoids nested file opens and keeps catalog/header lifetime consistent with
the current prefix-existence function.

## File Lifecycle

No file-format or companion-file change. The slice only changes how existing
`.mylite` pages are read.

## DDL Metadata Routing Impact

No metadata format change. Existing table and index-root catalog records are
reused.

## Embedded Lifecycle And API

No public `libmylite` API change. The first-party storage API behavior is
unchanged except for narrower internal materialization.

## Storage-Engine Routing

MyLite-routed durable tables benefit through existing prefix-existence callers.
Runtime-volatile MEMORY/HEAP behavior is unchanged.

## Build, Size, And Dependencies

No dependency or intended size-profile change. The implementation removes one
full-entryset fallback path and reuses existing helpers.

## Test Plan

- Extend storage unit coverage for prefix-existence checks when a published
  leaf root has append-tail inserts, changed-key updates, deletes, and missing
  prefixes.
- Run storage unit tests, storage-smoke, clang-format diff, and `git diff
  --check`.

## Acceptance Criteria

- `mylite_storage_index_prefix_exists_for_index()` no longer materializes a
  full index entryset for append-tail or no-root fallback prefix checks.
- Existing `skip_row_id` behavior is preserved.
- Static no-tail leaf roots keep their allocation-free existence path.
- Storage tests cover insert, update, delete, and missing-prefix fallback
  behavior.

## Verification Results

2026-05-23, macOS arm64 local worktree:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --preset storage-smoke-dev --output-on-failure
```

All passed.

## Risks And Unresolved Questions

- The fallback still scans append-tail pages until B-tree navigation or a more
  direct prefix-exists overlay exists.
- A later follow-up,
  [Allocation-Free Prefix Existence Overlay](../allocation-free-prefix-existence-overlay/specs.md),
  replaces the narrowed entryset allocation with row-id-only tracking.
