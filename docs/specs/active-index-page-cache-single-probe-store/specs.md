# Active Index Page Cache Single-Probe Store

## Problem

Prepared insert profiling after the branch-refold entryset cache still shows
the routed `ENGINE=InnoDB` insert step dominated by MyLite storage index-page
maintenance. A local sample of
`mylite_perf_baseline --phase=prepared-insert-components --profile-iterations=200000 1000`
shows repeated active index page cache store probes under:

- `insert_branch_index_leaf_entry()` ->
  `pager_write_buffered_maintained_root_or_branch_page()` ->
  `store_active_index_leaf_page_from_pager_write()`.
- `refold_branch_index_root_insert()` ->
  `write_index_branch_snapshot_pages()` ->
  `pager_write_prevalidated_index_leaf_page()` ->
  `store_active_index_leaf_page_from_encoded_page()`.
- `store_active_index_branch_page_from_pager_write()` during branch child
  refresh and buffered maintained-page writes.

The current store path can scan the same cache multiple times for one write:
the active store checks whether the page exists for limit enforcement,
`put_*_cache_entry()` checks again for replacement, and
`append_*_cache_entry()` checks once more before appending. With active caches
allowed to hold up to 1024 pages, repeated misses and appends turn into a
visible hot-path cost.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code and does not change MariaDB-derived SQL, handler, or
  parser code.
- `packages/mylite-storage/src/storage.c::store_active_index_leaf_page_for_file()`
  and `store_active_index_branch_page_for_file()` both do an existence lookup
  before calling a generic `put_*_cache_entry()` helper.
- `put_index_leaf_page_cache_entry()` and
  `put_index_branch_page_cache_entry()` do their own mutable lookup and then
  call append helpers that repeat the lookup on a miss.
- Durable leaf-cache insertion still needs duplicate protection because it can
  be called from read paths. Active pager-write stores already know whether the
  page was found once they have done the mutable lookup.

## Design

Split leaf page cache insertion into two layers:

1. A new append helper that appends a page known not to exist in the cache.
2. The existing duplicate-safe helper that preserves durable-cache
   append behavior by doing one lookup before calling the unchecked append.

Branch page caches only have active write-path callers, so they can use a
single unchecked append helper after the active store has already proved the
page is absent.

Then rewrite active leaf and branch store helpers to:

1. Resolve the mutable cache entry once.
2. Replace it immediately on a hit.
3. Evict the oldest entry only when the miss would exceed the active cache
   limit.
4. Append through the unchecked helper.

The change is intentionally internal. It preserves cache keys, entry metadata,
page copies, eviction policy, and cache lifecycle.

## Compatibility Impact

No SQL-visible or public API behavior changes. `ENGINE=InnoDB`, MyLite, and
other routed-engine paths keep the same page writes and cached page contents.
The change only removes redundant in-process cache scans while maintaining
the existing bounded active cache behavior.

## Single-File And Lifecycle Impact

No file-format or companion-file changes. Active statement caches remain
transient process-local heap state and are still cleared by the existing
rollback, catalog invalidation, statement-free, and reusable-statement reset
paths.

## Binary-Size And Dependency Impact

No dependency changes. The slice adds small first-party helper functions and
removes redundant cache lookup work on the hot write path.

## Test And Verification Plan

- Add storage test hooks that store repeated leaf and branch pages and prove
  active cache replacement, eviction, and latest-entry lookup still work.
- Run `mylite_storage_test` and `ctest -R mylite-storage`.
- Run storage-smoke embedded engine tests.
- Run `git diff --check` and `git clang-format --diff`.
- Run the prepared insert component benchmark before and after the change.

## Acceptance Criteria

- Active leaf and branch cache stores do one lookup for the ordinary hit or
  miss path.
- Existing durable duplicate-safe cache append behavior remains intact.
- Active cache replacement and eviction preserve page bytes and decoded page
  metadata.
- Relevant tests, formatting checks, and the prepared insert component
  benchmark pass.

## Risks

- Removing duplicate checks from the wrong helper could allow duplicate cache
  entries. The unchecked append helper must only be called after a caller has
  already proven the page is absent.
- The benchmark may remain dominated by checksum and page-encoding cost after
  this slice. That would make the next roadmap step a checksum or page-write
  batching slice, not more cache lookup cleanup.
