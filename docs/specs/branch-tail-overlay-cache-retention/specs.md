# Branch Tail Overlay Cache Retention

## Problem

Prepared insert profiling after direct branch-leaf insertion still shows time
under branch-tail overlay planning:

- `plan_maintained_index_root_inserts()`
- `plan_branch_index_root_insert()`
- `try_plan_branch_leaf_range_insert_redistribution()`
- `index_branch_tail_has_live_overlay()`
- `read_page_at()`

The live-overlay guard is required for current branch split, promotion, refold,
and redistribution plans because row-state overlays can hide rows that still
exist in older static branch leaves. A broad attempt to remove the guard would
let redistribution run after a same-table row-state page and violates existing
overflow-tail rollback coverage.

The safer issue is cache retention. The active branch-tail overlay cache is
bounded at 16 entries and clears the whole set when the next branch shape is
stored. Workloads that touch more than 16 branch boundaries lose all previously
verified absent-tail ranges and start scanning old tail pages again.

## Source Findings

- Base line: MariaDB 11.8.6, commit
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB refs:
  - `mariadb/sql/sql_insert.cc:Write_record::single_insert()` dispatches
    accepted inserts to `table->file->ha_write_row()`.
  - `mariadb/sql/handler.cc:handler::ha_write_row()` wraps the engine
    `write_row()` call after in-server constraint checks.
  - `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` prepares
    row payloads and key tuples, then calls
    `mylite_storage_append_row_with_index_entries()` for durable rows.
- MyLite refs:
  - `packages/mylite-storage/src/storage.c:index_branch_tail_has_live_overlay()`
    scans append-tail pages after a branch subtree's maximum child page id.
  - `packages/mylite-storage/src/storage.c:store_branch_tail_overlay_cache()`
    currently calls `clear_branch_tail_overlay_caches()` when the cache reaches
    `MYLITE_STORAGE_BRANCH_TAIL_OVERLAY_CACHE_LIMIT`.
  - `packages/mylite-storage/src/storage.c:advance_branch_tail_overlay_caches_after_branch_insert()`
    advances absent entries after maintained branch inserts suppress fallback
    index-entry pages for that index.

## Design

Keep live-overlay semantics unchanged and improve retention:

- raise the active branch-tail overlay cache limit from 16 to 256 entries;
- replace clear-all-on-limit behavior with single-entry oldest eviction;
- reset the last-lookup hint after eviction, because the entries array shifts;
- keep cache entries statement-local and keep existing cache matching,
  present-overlay, absent-overlay, and cache-advance semantics unchanged.

This avoids erasing unrelated verified branch-tail ranges when a statement
touches a broader set of branch boundaries. The memory bound stays small:
hundreds of compact cache entries per active statement, no persistent storage,
and no new dependency.

## Compatibility Impact

No SQL, public C API, metadata, storage-routing, or file-format behavior
changes. Routed `ENGINE=InnoDB`, `MyISAM`, `Aria`, `MEMORY`, and `HEAP`
behavior remains unchanged.

## Single-File And Embedded Lifecycle

The cache is in-memory statement scratch state. It is freed with the active
statement, savepoint, transaction, or reusable read statement cleanup. No
primary-file bytes or companion files change.

## Public API, Binary Size, And Dependencies

No public API or dependency impact. Binary-size impact is limited to one small
eviction helper and a test hook.

## Tests And Verification Plan

- Add a storage test-hook regression that stores more cache shapes than the
  cache limit and verifies:
  - the cache count remains bounded at the configured limit,
  - old entries are evicted individually rather than clearing all entries, and
  - newer entries remain findable after the limit is exceeded.
- Run:
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Acceptance Criteria

- Reaching the branch-tail overlay cache limit evicts one entry instead of
  clearing every cached range.
- Cache count remains bounded.
- Entries added after the limit remain findable.
- Existing row-state and index-entry live-overlay safety tests continue to
  pass.
- Local prepared-insert component timing improves or records the next measured
  bottleneck.

## Verification

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in 150.99 seconds.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R
  libmylite.embedded-storage-engine --output-on-failure` passed in
  33.21 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 10000` reported prepared insert step
  at 87.177 us/op.
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c`

## Risks

- A larger per-statement cache increases peak scratch memory. The limit is still
  fixed, small, and freed with statement lifecycle cleanup.
- Oldest-entry eviction is intentionally simple. A future access-order policy
  may retain hot entries better, but it needs extra mutation on lookup.
