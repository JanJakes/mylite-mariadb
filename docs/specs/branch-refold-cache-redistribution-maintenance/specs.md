# Branch Refold Cache Redistribution Maintenance

## Problem

Prepared-insert sampling still shows `read_index_leaf_entries()` under branch
refold planning. The branch refold entryset cache survives simple branch leaf
rewrites, but branch leaf-range redistribution currently invalidates the cache
even though redistribution only changes how existing logical index entries are
spread across child leaves.

After a redistribution, the next refold-capacity probe can rebuild the full
logical entryset from branch leaves instead of reusing the cached entryset plus
the inserted row.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code only.
- `update_branch_refold_entryset_caches_after_branch_insert()` preserves the
  cached logical entryset for non-structural simple branch inserts by appending
  the inserted `(key, row_id)`.
- `redistribute_branch_index_leaf_range_entry()` reads a bounded child-leaf
  range, appends the inserted entry, rewrites those leaves, and refreshes
  branch fences without changing the branch root level or child set.
- The refold cache key validates table id, root page id, index number, branch
  level, key size, and total entry count before reuse.

## Design

Treat branch leaf-range redistribution as a cache-preserving logical insert:

- Keep excluding refold, root split, child-branch split, upper-branch split, and
  deeper branch insert plans from opportunistic cache maintenance.
- Allow `redistribute_leaf_range` plans that stay on the same branch root and
  first leaf page to append the inserted `(key, row_id)` to an existing cached
  refold entryset.
- Continue removing the cache when the entry is missing, metadata no longer
  matches, or appending the new entry fails.

## Compatibility Impact

No SQL-visible, API-visible, storage-routing, or durable file-format behavior
changes. The optimization only affects transient branch refold planning state.

## Single-File And Lifecycle Impact

No checkpoint, journal, lock, recovery, or companion-file changes. Cache
contents remain active-statement owned and are still cleared by existing
statement lifecycle paths.

## Binary-Size And Dependency Impact

No dependency changes. The slice only adjusts a first-party cache-preservation
predicate and hook coverage.

## Test And Verification Plan

- Extend branch refold entryset cache hook coverage so a redistribution plan
  appends the inserted entry and the incremented branch entry count can reuse
  the cache.
- Keep stale entry-count miss and table invalidation coverage.
- Run storage unit tests, storage-smoke embedded tests, formatting checks, and
  prepared-insert component benchmarks.

## Acceptance Criteria

- Branch leaf-range redistribution preserves an existing branch refold entryset
  cache by appending the inserted row.
- Structural branch insert paths still remove or replace caches.
- Cache reuse still requires matching branch metadata and entry count.
- Relevant tests and formatting checks pass.

## Risks

The cache entry may remain unsorted after appending an inserted row. That was
already accepted for simple branch inserts; refold snapshot preparation sorts
out-of-order entrysets before encoding durable leaf pages.
