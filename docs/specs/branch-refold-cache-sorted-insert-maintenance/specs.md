# Branch Refold Cache Sorted Insert Maintenance

## Problem Statement

Recent branch-refold cache maintenance preserves cached entrysets across simple
leaf inserts and leaf-range redistribution. The preserved entryset currently
appends the inserted logical index entry. That is correct for ascending inputs,
but repeated prepared inserts with cyclic secondary-key values can make the
cache unsorted. Later refold publication then has to build a raw-entry order
array before encoding leaf pages.

This slice keeps preserved branch-refold entrysets sorted by `(key, row_id)` so
cached refold inputs can stay on the direct leaf-encoding path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; it does not change upstream MariaDB
  behavior.
- `prepare_index_leaf_pages()` already preserves the direct identity order when
  `build_raw_index_entry_order_if_needed()` sees a sorted entryset.
- `update_branch_refold_entryset_cache_after_simple_branch_insert()` is the
  cache-maintenance point used after simple branch-leaf inserts and eligible
  leaf-range redistribution.
- `tools/mylite_perf_baseline.c` uses cyclic secondary-key values in
  `benchmark_prepared_insert_components()`, which makes append-only cache
  maintenance a realistic source of out-of-order refold inputs.

## Design

- Add a narrow helper that inserts one `mylite_storage_index_entry` into an
  existing raw entryset in sorted `(key, row_id)` order.
- Keep key bytes append-only inside the raw byte buffer and reorder only the
  metadata arrays. Existing entryset consumers already use offsets rather than
  physical key-byte order.
- Use the sorted insert helper only for branch-refold cache maintenance. Leave
  the general append helper unchanged for callers that intentionally collect
  unsorted entries and sort later.
- If cache maintenance cannot grow the entryset, discard that cache entry as
  the existing preservation path already does.

## Compatibility Impact

No SQL, metadata, storage-engine routing, wire-protocol, or public C API
behavior changes. This is an internal performance-preservation change for
already-supported routed storage behavior.

## Single-File And Lifecycle Impact

No file-format, durable page, companion-file, locking, or recovery behavior
changes. The preserved cache is statement/runtime-local and can be rebuilt from
the primary `.mylite` file.

## Binary Size And Dependencies

No dependency changes. The code-size impact is limited to one small first-party
helper and an expanded unit hook.

## Test And Verification Plan

- Extend the branch-refold entryset cache unit hook with a middle-key
  redistribution insert.
- Verify the cached row-id/key order remains sorted.
- Verify `build_raw_index_entry_order_if_needed()` does not allocate an order
  array for the maintained cache.
- Run the first-party storage test target, storage CTest selection,
  storage-smoke embedded storage-engine test, formatting checks, and the
  prepared-insert component performance baseline.

## Acceptance Criteria

- Preserved branch-refold caches insert new logical entries in sorted order.
- Existing cache invalidation behavior is unchanged for structural branch
  rewrites and allocation failures.
- The regression test proves middle-key maintenance remains sorted and avoids
  raw-order construction.
- Targeted tests and checks pass.

## Risks

- This assumes branch-refold entrysets stored from refold reads are already
  sorted. That is consistent with the maintained leaf-page encoding path and is
  now preserved by cache maintenance.
