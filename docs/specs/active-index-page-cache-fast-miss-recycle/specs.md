# Active Index Page Cache Fast Miss Recycle

## Problem

Fresh prepared-insert profiling shows active index page cache maintenance under
branch snapshot writes as a remaining cost. Each newly encoded branch snapshot
leaf is stored in the root active statement cache. When page ids are increasing,
the cache still linearly searches all retained pages before discovering a miss.
Once full, eviction shifts the whole cache array before appending the new page.

That makes an otherwise bounded opportunistic cache add avoidable O(cache-size)
work to branch refold and snapshot write paths.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code only.
- `write_index_branch_snapshot_pages()` writes freshly encoded leaf pages
  through `pager_write_prevalidated_index_leaf_page()`, which refreshes the
  active leaf-page cache from encoded page metadata.
- `store_active_index_leaf_page_for_file()` and
  `store_active_index_branch_page_for_file()` first probe existing cache entries
  linearly and then call an oldest-entry eviction helper when the cache is full.
- The cache is opportunistic: readers treat misses as normal and fall back to
  durable or pager reads.

## Design

Add lightweight cache metadata for index leaf and branch page caches:

- Track the maximum page id ever inserted into the cache. A lookup for a page id
  greater than that high-water mark is a guaranteed miss and can skip the linear
  scan.
- Track a next replacement slot. When an active cache reaches its entry limit,
  reuse that slot in place instead of freeing the first page and shifting all
  entries.
- Preserve existing update behavior when the page id is already cached.
- Keep durable cache behavior append-only; the high-water mark only helps miss
  checks there and does not change durability or invalidation policy.

## Compatibility Impact

No SQL-visible, API-visible, storage-routing, or durable file-format behavior
changes. Active index page caches remain transient and opportunistic.

## Single-File And Lifecycle Impact

No checkpoint, journal, lock, recovery, or companion-file changes. Statement
cleanup still frees the entire active cache, including reused page buffers.

## Binary-Size And Dependency Impact

No dependency changes. The slice adds a few fields and small helper functions in
first-party storage code.

## Test And Verification Plan

- Extend active index page cache hook coverage for leaf and branch caches:
  existing-page stores still take a single lookup and update in place, ascending
  new page ids remain bounded, and a full active cache recycles page buffers
  instead of shifting or reallocating the page copy.
- Run storage unit tests, storage-smoke embedded tests, formatting checks, and
  prepared-insert component benchmarks.

## Acceptance Criteria

- Lookup of a page id above the cache high-water mark returns a miss without a
  linear entry scan.
- Full active leaf and branch page caches reuse bounded slots in place.
- Existing cached page updates still replace the correct entry.
- Relevant tests and formatting checks pass.

## Risks

The high-water mark is intentionally monotonic and can be stale after slot
reuse. That is safe: a stale high-water mark only disables the fast miss for
lower page ids; it never reports a false miss for a page id that could be cached.
