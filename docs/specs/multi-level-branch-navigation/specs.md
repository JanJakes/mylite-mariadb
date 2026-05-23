# Multi-Level Branch Navigation

## Problem

MyLite branch pages already carry a `level` field and child high-key fences, but
storage readers reject branch roots above level `1`. That blocks the next
storage-index milestone: splitting a full single-level branch root into a
root page that points at lower branch pages instead of falling back to
append-tail scans.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc` routes exact and prefix index reads through
  `handler::ha_index_read_map()` and same-key iteration through
  `handler::ha_index_next_same()`. MyLite must keep returning ordered cursor
  rows for those handler calls.
- `mariadb/storage/mylite/ha_mylite.cc` uses storage-level exact and prefix
  entryset reads in `ha_mylite::index_read_map()` and
  `ha_mylite::index_read_idx_map()`, then depends on ordered entries for
  `ha_mylite::index_next()` and `ha_mylite::index_next_same()`.
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `encode_index_branch_page()` and `decode_index_branch_page()` already store
  and validate branch `level`, page-owned entry count, child page ids, and
  ordered child high `(key, row_id)` fences.
- `read_index_branch_leaf_run_root()`,
  `find_index_branch_leaf_run_match_page()`, and
  `find_index_branch_leaf_run_prefix_lower_page()` currently require
  `branch_page->level == 1U`, so a structurally valid higher-level branch root
  is treated as corrupt.

## Scope

- Decode higher-level branch pages without applying the single-level
  leaf-capacity entry bound.
- Teach published branch-root readers to recursively collect leaf child page ids
  from branch pages whose levels decrease by one at each branch edge.
- Keep the current bounded `mylite_storage_index_leaf_run` child-id array for
  this slice; reject branch trees whose collected leaf count exceeds that
  bound.
- Use recursive branch navigation for exact-key and prefix lower-bound lookup
  when the root level is greater than `1`.
- Keep full index reads ordered by traversing leaves in branch-cell order.
- Set the append-tail overlay start after the highest static page id in the
  branch subtree, including intermediate branch pages.

## Non-Goals

- No writer-side branch-page-full root split in this slice.
- No branch-page split, merge, redistribution, or arbitrary-depth fanout beyond
  the current in-memory leaf-run bound.
- No row-DML maintenance for multi-level branch roots.
- No variable-width, nullable, collation-aware, FULLTEXT, SPATIAL, vector, or
  expression-index branch keys beyond the existing fixed raw byte-key support.
- No SQL-visible behavior or public API change.

## Compatibility Impact

Supported exact, prefix, and full index reads continue to return the same
ordered row ids and key bytes for MariaDB handler callers. The new behavior is
storage-internal: a catalog-published branch root with level greater than `1`
can now serve the same read shapes as an equivalent single-level branch root.

Unsupported multi-level branch-root row DML remains on the existing fallback
paths until writer maintenance is implemented.

## Single-File And Lifecycle Impact

All branch and leaf pages remain durable pages in the primary `.mylite` file.
No companion files are introduced. The immutable published branch subtree stays
part of the catalog-root snapshot, and later append-only visibility overlays
start after the highest page id in that subtree.

## File-Format Impact

No new page type or field is added. The reader starts accepting the existing
`TABLE_INDEX_BRANCH` page type with `level > 1` for bounded published-root
reads.

`decode_index_branch_page()` keeps validating page-local shape, child page ids,
and ordered child fences. The single-level maximum entry-count bound becomes a
level-aware bound so higher-level pages can represent more entries than their
direct child count times one leaf capacity.

## Test Plan

- Add a storage unit test that rebuilds a fixed-width published branch root,
  appends two lower branch pages, rewrites the root as a level-`2` branch page,
  and points each lower branch page at a subset of the existing leaf pages.
- Verify exact lookup, prefix entryset lookup, prefix-exists lookup, and full
  index reads through the level-`2` root.
- Corrupt an unrelated non-candidate leaf page after clearing durable caches and
  verify an exact lookup in another branch child still succeeds through branch
  navigation.
- Verify append-tail overlay visibility still starts after the highest static
  branch-subtree page by appending another indexed row after publishing the
  manual level-`2` root.
- Keep existing single-level branch tests passing.
- Run:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --preset storage-smoke-dev --output-on-failure
```

## Verification Results

Passed on 2026-05-23:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --preset storage-smoke-dev --output-on-failure
```

## Acceptance Criteria

- A bounded multi-level branch root can serve exact, prefix, prefix-exists, and
  full index reads without falling back to append-history scans.
- Branch readers validate branch levels, table id, index number, key size, leaf
  count, and leaf page contents before returning results.
- Append-tail overlays after a multi-level branch subtree remain visible.
- Existing single-level branch-root insert/update/delete/read behavior remains
  unchanged.
- Roadmap and storage architecture docs describe multi-level branch reads as
  implemented while keeping root splits and broad B-tree maintenance planned.

## Risks And Follow-Ups

- The current leaf-run representation still stores branch leaf ids in a bounded
  fixed array. That is enough for the first root-split milestone, but a real
  production B-tree cursor should avoid materializing every leaf id before
  lookup.
- Writer-side root splitting should follow immediately: when a full
  single-level branch root needs one more child, publish a level-`2` root with
  lower level-`1` branch pages and prove rollback/recovery.
- Multi-level branch row-DML maintenance, merge/redistribution, and broader
  write-concurrency behavior remain separate slices.
