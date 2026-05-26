# Active Index Page Cache Metadata Store

## Problem

After active index page cache stores were reduced to one lookup, a fresh
prepared insert sample still shows significant time under transient active
cache refresh:

- `store_active_index_leaf_page_from_pager_write()` spends sampled time in
  `decode_index_leaf_page()` -> `checksum_page()`.
- `store_active_index_branch_page_from_pager_write()` spends sampled time in
  `decode_index_branch_page()` -> `checksum_page()`.
- Refold snapshot leaf writes already use
  `store_active_index_leaf_page_from_encoded_page()`, which reads page-header
  metadata without a full checksum decode.

These active caches are process-local acceleration structures populated
immediately after MyLite writes a page. Revalidating the just-written page with
the full durable decoder burns hot-path CPU without adding durable safety: if
the internal writer produced corrupt bytes, the file write itself is already
wrong. The cache only needs enough metadata to avoid returning impossible page
shapes later.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code and does not change MariaDB-derived SQL, handler, or
  parser code.
- `packages/mylite-storage/src/storage.c::store_active_index_leaf_page_from_encoded_page()`
  already trusts encoded leaf pages enough to parse page id, table id, index
  number, key size, entry count, and used bytes without recomputing the page
  checksum.
- `store_active_index_leaf_page_from_pager_write()` and
  `store_active_index_branch_page_from_pager_write()` still call the full
  durable decoders only to populate active cache metadata.
- Active cache readers reconstruct decoded page structs from stored metadata
  and page bytes. They do not need row-order or child-fence proof from cache
  population because the write path already constructed or decoded the page
  before writing.

## Design

Add small metadata readers for active cache storage:

- `read_index_leaf_page_cache_metadata()` reads and validates leaf page header
  fields, capacity, and used-byte shape without checksum recomputation.
- `read_index_branch_page_cache_metadata()` does the same for branch page
  header fields, level, child count, entry count, capacity, and used-byte
  shape.

Then route both active pager-write cache refresh helpers through those metadata
readers. The existing durable read decoders stay unchanged and continue to
verify checksums, row ordering, child addresses, and branch fences when reading
from disk.

## Compatibility Impact

No SQL-visible or public API behavior changes. Routed `ENGINE=InnoDB`, MyLite,
and other storage-engine aliases continue to write the same page bytes. The
change only avoids redundant validation before publishing transient active
cache entries in the same process.

## Single-File And Lifecycle Impact

No file-format or companion-file changes. Active cache entries remain scoped
to statements and are still cleared by rollback, catalog invalidation,
statement cleanup, and reusable-statement reset paths.

## Binary-Size And Dependency Impact

No dependency changes. The slice adds small first-party metadata readers and
removes hot checksum work from active cache refresh.

## Test And Verification Plan

- Extend storage test hooks so leaf and branch pager-write cache refreshes
  populate active caches without calling `checksum_page()`.
- Keep full storage and storage-smoke tests passing.
- Run `git diff --check` and `git clang-format --diff`.
- Run prepared insert component benchmarks before and after the change.

## Acceptance Criteria

- Active pager-write leaf and branch cache refreshes do not call full
  checksum-validating decoders.
- Malformed page headers still clear the affected active cache instead of
  storing impossible metadata.
- Durable disk reads continue to use full decoders and checksum validation.
- Relevant tests, formatting checks, and the prepared insert component
  benchmark pass.

## Risks

- Header-only metadata parsing must not be used for durable reads. It is only
  valid immediately after trusted MyLite page writes.
- This does not remove checksum cost from page encoding itself; if that remains
  dominant, the next slice should target checksum amortization or write
  batching rather than active cache refresh.
