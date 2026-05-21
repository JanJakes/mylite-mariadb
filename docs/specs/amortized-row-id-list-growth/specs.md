# Amortized Row-Id List Growth

## Problem

Internal storage row-id lists reserve exactly the new result count on every
append. That is unnecessary heap churn for append-history exact-index scans,
published-leaf append-tail overlays, and live-row collection paths that discover
matching row ids incrementally.

This is not the maintained B-tree or pager work, but it removes an avoidable
linear allocation pattern from current scan-backed paths.

## Source Findings

- Base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- No MariaDB source change is required. MariaDB continues to call through the
  MyLite handler/storage boundary for exact index probes, rowset reads, and
  cursor materialization.
- `packages/mylite-storage/src/storage.c::append_row_id_to_list()` is the
  common incremental row-id append helper.
- `grow_row_id_list_for_append()` currently reallocates to `count + additional`
  every time. Bulk leaf matches reduce one caller's append count, but scan
  fallback and tail overlay paths still append one discovered row id at a time.
- `mylite_storage_row_id_list` is internal to the storage implementation, so it
  can gain transient capacity without changing the public C ABI.

## Design

Track capacity on the internal row-id list:

- add a `capacity` field to `mylite_storage_row_id_list`;
- make `grow_row_id_list_for_append()` return immediately when current capacity
  can hold the requested append;
- grow from a small initial capacity and double geometrically until the
  requested count fits;
- preserve overflow checks before multiplying or allocating; and
- set capacity on copied row-id lists so later internal appends can reuse their
  allocation.

## Non-Goals

- No public API change.
- No file-format change.
- No change to row visibility, duplicate-key ordering, or append-tail overlay
  semantics.
- No row-id cache redesign, B-tree maintenance, or pager work.

## Compatibility Impact

SQL-visible behavior should not change. The same row ids are kept in the same
order; only transient allocation strategy changes.

## Single-File And Lifecycle Impact

All added state is process-local transient capacity on an internal list. The
slice adds no durable pages and no companion files.

## Tests

Existing storage tests cover exact-index scan fallback, published-leaf duplicate
reads, append-tail overlays, live-row caches, and rowset materialization. Keep
those passing after the allocation change.

Run at least:

```sh
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
git diff --check
```

## Acceptance Criteria

- Incremental row-id appends no longer realloc on every appended row id.
- Existing exact-index, rowset, live-row, and published-leaf storage tests pass.
