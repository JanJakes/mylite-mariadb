# Catalog Free-List Prepend Coalescing

## Problem

Catalog-chain reclamation and branch-leaf reclamation both publish reusable
page runs by prepending a new free-list root. Branch reclamation can already
coalesce a newly reclaimed lower-adjacent page with the current free-list root,
but catalog page-run reclamation still creates a separate run in the same
adjacent shape.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- Catalog reclamation already writes a free-list page at the reclaimed run's
  first page and publishes `header.free_list_root_page` under the recovery
  journal.
- If a reclaimed catalog run ends immediately before the current free-list root
  run, the reclaimed run can become the new root for the combined run by
  preserving the old root's `next_root_page` and increasing the run length.
- The existing current-root page is only read for this lower-adjacent prepend
  shape; it does not need to be dirtied or journaled.

## Scope

- Share the lower-adjacent prepend coalescing logic between branch leaf
  reclamation and catalog page-run reclamation.
- Coalesce only when `first_page_id + page_count ==
  current_root.run_start_page`.
- Keep non-adjacent reclamation on the existing prepend behavior.
- Keep the existing catalog recovery journal lifecycle.

## Non-Goals

- No arbitrary free-list search.
- No coalescing with a run after the current root run.
- No allocator policy changes.
- No file shrinking.

## Compatibility Impact

No SQL, public API, storage-engine routing, or MariaDB compatibility behavior
changes. This only changes internal free-list shape.

## Single-File And Lifecycle Impact

All reclaimed pages stay in the primary `.mylite` file. The free-list remains a
durable in-file chain of reusable page runs.

## Recovery Impact

Catalog reclamation keeps its existing journal boundary. The newly reclaimed
run's first page and header are restored on rollback or stale-journal recovery;
the previous free-list root page remains unmodified.

## Test Plan

- Add controlled storage-hook coverage for catalog page-run reclamation where a
  reclaimed run directly precedes the current free-list root run.
- Keep existing catalog free-list reuse and branch free-list coalescing tests.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Catalog page-run reclamation coalesces with a lower-adjacent current
  free-list root run.
- Branch leaf reclamation still uses the same coalescing behavior.
- Non-adjacent free-list publication remains unchanged.
- Docs distinguish root-prepend coalescing from broader future coalescing.

## Implementation Notes

- `encode_reclaimed_free_list_root_page()` now centralizes free-list root
  publication for reclaimed runs.
- The helper reads the current root run and folds it into the new root only when
  the reclaimed run ends exactly where the current root run begins.
- `reclaim_catalog_page_run()` and branch final-leaf reclamation both use the
  shared helper.
- Storage test hooks cover controlled catalog and branch coalescing fixtures,
  while existing public-path tests keep covering non-adjacent branch
  reclamation, rollback, and stale recovery.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Risks And Follow-Ups

- The free-list still only checks the current root run.
- Higher-adjacent and arbitrary-chain coalescing remain future work.
