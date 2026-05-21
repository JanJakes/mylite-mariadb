# Static Leaf First Row Id

## Problem

`mylite_storage_find_index_entry()` only needs one matching row id. When a
published index leaf run has no append tail, the current path still builds a
temporary row-id list from the leaf run and then returns the first row id from
that list. For primary-key and unique-key point reads over freshly published
leaf roots, that is avoidable allocation and copy work on the hot path.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- No MariaDB source change is required. The handler already routes exact
  storage probes through `mylite_storage_find_index_entry()`.
- `packages/mylite-storage/src/storage.c::find_exact_index_row_id()` checks the
  active exact-index cache, then a published leaf run, then the durable
  exact-index cache, and finally the append-history scan.
- `read_index_leaf_exact_row_ids()` correctly handles published leaf runs by
  collecting all base matches and overlaying append-tail row-state/index-entry
  pages. That full list is necessary when pages were appended after the leaf
  run because later deletes, replacements, or changed-key entries can change
  the first live match.
- When `leaf_run.tail_page_id == header->page_count`, the immutable leaf run is
  the full live index snapshot for that key. In that case, the first matching
  row id can be found directly from the leaf pages without building a result
  list.

## Design

Add a direct first-row-id helper for static published leaf runs:

- locate and validate the index-root record and leaf run as the existing leaf
  read path does;
- if the run has an append tail, decline the fast path and leave the existing
  list-plus-tail overlay path in place;
- when the run is static, binary-search the matching leaf page, walk left to
  the first page that can contain the duplicate key, and return the first
  matching row id directly;
- return `0` with a used-leaf flag when the static leaf run has no matching key;
  and
- keep the existing active-cache, durable-cache, and append-scan fallbacks for
  missing roots or append-tail cases.

## Non-Goals

- No change to append-tail overlay semantics.
- No new public API.
- No file-format change.
- No B-tree maintenance or pager work.

## Compatibility Impact

SQL-visible behavior should not change. The fast path applies only when the
published leaf run is already the complete live snapshot for the index. Tail
cases still use the existing overlay logic.

## Single-File And Lifecycle Impact

The slice only changes in-memory read selection over existing primary-file leaf
pages. It adds no durable state and no companion files.

## Tests

- Extend storage leaf coverage so `mylite_storage_find_index_entry()` returns
  the first matching row id from a freshly rebuilt duplicate-key leaf run with
  no append tail.
- Keep append-tail exact lookup coverage passing to prove the fast path does
  not bypass tail overlays.

Run at least:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
```

## Acceptance Criteria

- Static published leaf runs can satisfy first-row-id exact probes without
  allocating a row-id list.
- Published leaf runs with append tails keep using the existing overlay path.
- Existing storage tests pass.
