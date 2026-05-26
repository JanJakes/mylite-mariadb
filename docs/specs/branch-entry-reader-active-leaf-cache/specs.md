# Branch Entry Reader Active Leaf Cache

## Problem

Prepared insert profiling still shows `read_index_leaf_entries()` work under
branch refold planning. The previous branch refold entryset-cache slice keeps
more complete entrysets alive, but branch entry readers still reread and
rechecksum child leaf pages when those leaf pages were already written through
the active statement cache during the same statement.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code only.
- `read_index_leaf_entries()` delegates branch roots to
  `read_index_branch_entries()`, which materializes branch children through
  `append_index_branch_entries_to_entryset()`.
- Level-one branch readers call leaf append helpers that use
  `read_index_leaf_page()`. That path can use the durable leaf cache, then falls
  back to pager reads plus `decode_index_leaf_page()` checksum validation.
- Active leaf-page cache readers already exist for branch insert planning and
  writer paths, with metadata copied from trusted pager writes or encoded page
  buffers.

## Design

Introduce a local branch-leaf reader helper:

- First probe `read_cached_active_index_leaf_page()` for the requested leaf id.
- If the active cache contains the page, use the cached bytes and metadata
  directly.
- If the active cache misses, preserve the existing `read_index_leaf_page()`
  durable-cache and pager fallback behavior.
- Reuse the helper in branch leaf append paths that aggregate full, prefix, and
  limited branch leaf entries.

The helper is deliberately read-only. This slice does not populate the active
cache on read misses because pager writes already own active cache maintenance,
and widening cache population policy would need separate lifecycle review.

## Compatibility Impact

No SQL-visible, API-visible, storage-routing, or durable file-format behavior
changes. Branch readers still validate table id, index number, key size, page
order, entry counts, and prefix/limit bounds before returning entries.

## Single-File And Lifecycle Impact

No checkpoint, journal, lock, recovery, or companion-file changes. The active
cache remains bound to the root active statement and is cleared through existing
statement lifecycle paths.

## Binary-Size And Dependency Impact

No dependency changes. The helper is small first-party storage code.

## Test And Verification Plan

- Add storage-hook coverage that stores a valid index leaf page in the active
  leaf cache, aggregates it through the branch leaf entry reader, and verifies
  the page is consumed without checksum decode.
- Keep storage unit tests, storage-smoke embedded tests, formatting checks, and
  a prepared-insert component benchmark passing.

## Acceptance Criteria

- Branch leaf entry aggregation uses active cached leaf pages before durable or
  pager reads.
- Cache misses preserve the existing durable-cache and pager read behavior.
- Prefix and limited branch leaf readers share the same cached read path.
- Relevant tests and formatting checks pass.

## Risks

The optimization depends on active cache metadata remaining in sync with pager
writes. Existing active leaf-cache tests already validate metadata extraction
without checksum decode; this slice adds branch-reader coverage for the same
contract.
