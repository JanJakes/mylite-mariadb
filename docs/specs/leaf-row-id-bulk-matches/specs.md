# Leaf Row-Id Bulk Matches

## Problem

Published leaf-root exact reads have two result paths. The entryset path bulk
grows result arrays once per matching leaf page, but the row-id path still
calls `append_row_id_to_list()` for every matching leaf entry. That causes one
`realloc()` per match when a published secondary leaf page contains many rows
for the same exact key.

This is not the primary B-tree navigation work, but it is unnecessary overhead
on the current leaf-run exact lookup path.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- No MariaDB upstream behavior changes are needed. MariaDB still requests exact
  and indexed-row lookups through the MyLite handler/storage boundary.
- `packages/mylite-storage/src/storage.c::read_index_leaf_exact_row_ids()` reads
  a catalog-backed leaf run and calls
  `append_index_leaf_run_matches_to_row_id_list()` for the immutable base
  snapshot before scanning append-tail row-state and index-entry pages.
- `append_index_leaf_matches_to_row_id_list()` binary-searches the first
  matching entry in one leaf page, then appends each matching row id one at a
  time through `append_row_id_to_list()`.
- `append_index_leaf_matches_to_entryset()` already counts matches first and
  grows its output arrays once per leaf page.

## Design

Add a small row-id list growth helper and make
`append_index_leaf_matches_to_row_id_list()` follow the entryset path:

- keep the existing binary search for the first matching leaf entry;
- count the contiguous matching entries in the leaf page;
- grow the row-id list once for that match count;
- copy all matching row ids into the newly reserved range; and
- keep append-tail overlay handling unchanged.

The generic single-row `append_row_id_to_list()` remains available for scan
paths where matches are discovered one page at a time and where row-state
operations interleave with index-entry pages.

## Non-Goals

- No public API change.
- No file-format change.
- No change to leaf-run ordering or duplicate-key semantics.
- No B-tree split, maintenance, or pager work.

## Compatibility Impact

SQL-visible behavior should not change. The same matching row ids are returned
in the same leaf order; only allocation granularity changes.

## Single-File And Lifecycle Impact

All state remains in memory while reading the existing primary `.mylite` file.
The slice adds no durable state and no companion files.

## Tests

The existing storage leaf-run tests cover exact duplicate reads across
published leaf pages and append-tail overlays. Keep those tests passing after
the allocation change.

Run at least:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
```

## Acceptance Criteria

- Published leaf row-id exact matches grow the result list once per matching
  leaf page rather than once per row id.
- Existing exact row-id, exact entryset, and append-tail overlay behavior stays
  covered by storage tests.
