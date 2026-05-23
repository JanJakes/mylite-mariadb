# Active Row-Only Rewrite Buffer Lookup

## Problem

Repeated row-only updates inside one active storage checkpoint mostly use the
buffered rewrite path: after the first update appends a replacement row and
row-state page, later same-row updates can rewrite the buffered row page in
place with rollback preimages. Current code first resolves the buffered
row/state range in `rewrite_active_update_pages()`, then calls the row-only
helper, which repeats buffered page lookups for the same row and state pages.

The duplicate lookups are small, but they sit directly in the hot
storage-level row-only update mutation path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` dispatches
  preserving-index row-only updates to
  `mylite_storage_update_row_preserving_index_entries_in_statement()` when a
  MyLite statement checkpoint is active.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries_for_context()`
  calls `rewrite_active_update_pages()` before appending replacement pages.
- `packages/mylite-storage/src/storage.c::rewrite_active_update_pages()` already
  resolves the buffered append range for the row page and row-state page.
- `packages/mylite-storage/src/storage.c::rewrite_active_row_only_update_page()`
  currently resolves the same row page, and on first shape validation resolves
  the row-state page again.

## Design

Pass the already-resolved row page reference and row-state page pointer from
`rewrite_active_update_pages()` into `rewrite_active_row_only_update_page()`.
The helper still owns all row-only validation and rollback preimage capture,
but it no longer performs duplicate append-buffer lookups for pages the caller
has already proven are resident in the contiguous rewrite range.

## Scope

This is a storage hot-path refactor only. It does not change the durable file
format, rollback semantics, page validation rules, handler behavior, SQL
semantics, public APIs, or index maintenance.

## Compatibility Impact

No user-visible compatibility change. The same storage tests continue to cover
statement rollback, transaction rollback, savepoint rollback, and stale journal
recovery for active row-only rewrites.

## Storage And Lifecycle Impact

Durable and transient storage lifecycle remains unchanged. The active
append-buffer rewrite still captures per-statement undo before mutating buffered
pages and keeps row-state validation in the same helper.

## Verification Plan

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-row-update-components 10000 200000`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-row-only-update-components 10000 200000`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `git diff --check`

## Acceptance Criteria

- Storage-smoke tests pass.
- The row-only update benchmarks continue to pass and show no regression.
- The diff is limited to the active row-only rewrite helper and documentation.

## Risks

The expected improvement is modest because this removes control-plane lookups,
not the dominant MariaDB prepared update overhead. If timings are noisy, the
change is still acceptable only if tests pass and the code path is simpler.
