# Live Row Cache Amortized Growth

## Problem

Durable and active complete live-row caches reuse compacted row-id lists for
unchanged full-row and count reads. Their mutation path still grows the owned
row-id array to the exact new count for every maintained insert. A transaction
that appends many rows to a seeded live-row cache therefore pays avoidable
`realloc()` churn even though the cache is bounded and internal.

This is separate from maintained row directories and B-tree navigation. It
keeps the current complete-list cache cheaper while those larger structures are
still planned.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- No MariaDB source change is required. The live-row cache is first-party
  MyLite storage state under `packages/mylite-storage/src/storage.c`.
- `mylite_storage_live_row_id_cache` owns a copied row-id array for one table
  and durable header fingerprint, but only tracks `count`.
- `append_row_id_to_cache()` deduplicates linearly, then calls `realloc()` for
  `count + 1` before appending the new row id.
- The cache already has a hard entry bound through
  `MYLITE_STORAGE_DURABLE_LIVE_ROW_ID_ENTRY_LIMIT`, so retaining extra capacity
  remains bounded.

## Design

Track transient capacity on each complete live-row cache:

- add a `capacity` field beside `count`;
- grow the array geometrically when `count + 1` exceeds capacity;
- cap retained capacity at `MYLITE_STORAGE_DURABLE_LIVE_ROW_ID_ENTRY_LIMIT`;
- preserve the existing duplicate check and overflow handling; and
- copy capacity from the internal row-id-list copy used by cache assignment.

## Non-Goals

- No public API change.
- No file-format change.
- No change to live-row visibility, cache invalidation, or active cache
  promotion rules.
- No hash index for this complete-list cache; the separate active
  validation-cache buckets already cover the hot validation-membership path.

## Compatibility Impact

SQL-visible behavior should not change. The same complete row-id list is
maintained and promoted under the same invalidation rules; only allocation
granularity changes.

## Single-File And Lifecycle Impact

All added state is process-local transient capacity on internal caches. The
slice adds no durable pages, journals, locks, or companion files.

## Tests

Existing storage tests cover durable live-row cache reuse, active live-row list
maintenance, mutation invalidation, transaction rollback, and rowset/count
results. Keep those passing after the allocation change.

Run at least:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
```

## Acceptance Criteria

- Complete live-row caches no longer reallocate on every maintained row append.
- Existing live-row cache, rowset, count, and transaction storage tests pass.
