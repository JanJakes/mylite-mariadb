# Batched Buffered Range Undo Capture

## Problem

Prepared primary-key updates that change one fixed-width secondary key rewrite
two active append-buffer pages: the current row page and the changed index-entry
page. Earlier slices reduced each preimage to the checksum-plus-payload/key
range, but the same-size single-index path still captures those two ranges
through two independent helper calls.

That repeats the empty undo-list setup and duplicate-detection path in the
hot nested statement even though the cached single-index rewrite shape already
knows the row page and changed index page are distinct pages in the active
append buffer.

## Source Findings

- MariaDB base line: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The affected code is first-party MyLite storage in
  `packages/mylite-storage/src/storage.c`.
- `rewrite_active_update_pages()` dispatches cached one-index rewrites to
  `rewrite_active_single_index_update_page()` after row, row-state, and
  changed index-page shape validation has been cached.
- `capture_buffered_page_undo_pair_from_pages()` already batches two prefix
  preimages for the empty undo-list hot path.
- `capture_buffered_page_undo_range_from_page()` already captures compact
  range preimages and upgrades a compact page undo to a prefix undo when a later
  same-statement rewrite needs the conservative restore path.
- Existing storage coverage exercises same-size single-index rollback after
  checksum refresh and same-savepoint upgrade to the size-changing path.

## Design

1. Add a two-page range undo helper that shares the batched empty-list fast path
   with the existing two-page prefix helper.
2. Implement the existing prefix helper as the zero-offset case of the new
   range helper.
3. Preserve conservative fallback behavior for non-empty undo lists, bucketed
   lists, or same-page requests by using the existing single-page capture
   helpers.
4. Route same-size cached one-index rewrites through the range-pair helper so
   the row checksum/payload range and changed index checksum/key range are
   captured together before either page is mutated.

The helper does not change rollback ordering: row page preimage first, then
index-entry page preimage.

## Affected Subsystems

- MyLite storage active append-buffer update rewrites.
- Statement/savepoint rollback preimage capture for buffered pages.
- Prepared-update storage performance baseline.

No MariaDB SQL, handler routing, catalog, public API, or wire-protocol code is
changed.

## Compatibility Impact

No SQL behavior or storage file-format behavior changes. The optimization only
changes transient rollback bookkeeping for pages that already live in the
active append buffer.

Statement rollback must restore the same bytes and checksum-dirty state as the
previous two independent range captures.

## Single-File And Embedded Lifecycle Impact

No durable state, companion file, lock, recovery-journal format, or embedded
lifecycle change.

## Public API Or File-Format Impact

No public `libmylite` API, first-party storage API, or `.mylite` file-format
change.

## Binary-Size Impact

The slice adds a small first-party helper and reuses existing undo entry
storage. It adds no dependency and should have negligible binary-size impact.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run focused storage-engine CTest coverage.
- Run full storage-smoke CTest if the focused checks pass.
- Run `git diff --check` and `git clang-format --diff` on
  `packages/mylite-storage/src/storage.c`.
- Run prepared-update component and full prepared-update baselines.
- Sample a focused prepared-update run and confirm the hot path uses the
  range-pair helper for cached same-size one-index rewrites.

## Acceptance Criteria

- Same-size cached one-index rewrites capture row and index range preimages via
  one batched helper on the ordinary empty-list path.
- Existing prefix pair undo behavior is preserved through the zero-offset
  wrapper.
- Same-page, non-empty undo-list, and bucketed undo-list fallbacks stay
  conservative.
- Existing storage rollback and embedded storage-engine tests pass.

## Verification Evidence

- `git diff --check`: passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`: no
  changes.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed before and after the MariaDB storage-smoke archive relink.
- `ctest --preset storage-smoke-dev --output-on-failure`: passed, 10/10.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build build libmariadbd.a`: passed with existing
  no-symbol archive warnings.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000`: prepared update step
  measured `2.295 us/op` in one focused run after the change.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 10000 1000000`: prepared updates measured
  `2.316 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=storage-row-update-components 10000 1000000`: storage update mutation
  measured `0.417 us/op`.
- A one-second macOS `sample` run over prepared update components showed the
  storage rewrite frame still present, with no separate single-page range
  capture helper frames visible under the same-size cached one-index path.

## Risks And Unresolved Questions

- This reduces helper overhead inside the remaining storage rewrite frame but
  does not change MariaDB prepared execution, row materialization, or index
  navigation. Further SQLite-like performance still requires the navigable
  index and pager work already called out in the roadmap.
