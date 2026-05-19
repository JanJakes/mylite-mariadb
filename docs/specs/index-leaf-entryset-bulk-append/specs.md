# Index Leaf Entryset Bulk Append

## Problem

After durable row and leaf-page caches, repeated published leaf-root exact
secondary reads spend visible time in `append_index_entry_to_entryset()` and
allocator frames. The benchmark key has many matching row ids per exact value,
so `append_index_leaf_matches_to_entryset()` currently reallocates the keys,
offsets, sizes, and row-id arrays once per matched entry.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` calls
  `mylite_storage_read_exact_index_entries()` for exact secondary cursors.
- `packages/mylite-storage/src/storage.c::read_index_leaf_exact_entries()`
  uses published leaf roots when available.
- `append_index_leaf_matches_to_entryset()` binary-searches the first matching
  key in a decoded leaf page, then appends each matching row id through
  `append_index_entry_to_entryset()`.
- `append_index_entry_to_entryset()` grows four arrays with `realloc()` for
  every appended entry.

## Design

- Add an internal helper that grows an index entryset once for a known number
  of appended entries and appended key bytes.
- Keep the public `mylite_storage_index_entryset` layout unchanged; the helper
  does not introduce persistent capacity fields.
- Update single-entry appends to use the same helper.
- Update leaf-page exact-match appends to count contiguous matches in the leaf
  page, grow the entryset once, then fill key offsets, key sizes, row ids, and
  key bytes in one pass.
- Preserve overflow checks before all size calculations.

## Compatibility Impact

SQL-visible behavior is unchanged. Entrysets contain the same keys and row ids
in the same order as before; only allocation strategy changes.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The change is transient
in-memory construction of storage API results.

## Test And Verification Plan

- Existing storage tests cover exact index entrysets, leaf-root exact lookups,
  mutation overlays, and free behavior.
- Rebuild and run the storage unit tests.
- Rebuild the storage-smoke target and run the storage-engine compatibility
  harness.
- Run the local performance baseline to measure secondary exact-select impact.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Published leaf-root exact entryset reads avoid per-entry array reallocations
  for matching leaf-page runs.
- Storage and storage-engine compatibility tests pass.
- Secondary exact-select benchmark timings improve or stay within noise while
  preserving checksum results.

## Risks

- This does not solve repeated SQL planning, table-definition reads, or full
  pager work. It removes one allocation-heavy storage-local cost from exact
  leaf reads.
