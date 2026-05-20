# Typed Buffered Page Undo Capture

## Problem

Repeated active buffered updates must capture per-statement preimages before
rewriting unpublished row and changed index-entry pages. The generic undo
capture helper currently rediscoveres the page type and meaningful byte prefix
from the full page image for every first capture in a nested statement.

The focused prepared-update profile now shows the next storage hotspot under
`capture_buffered_page_undo()` and `buffered_page_undo_used_size()` after
earlier buffered rewrite and active-cache maintenance reductions.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite owns the single-file storage and active checkpoint implementation in
  `packages/mylite-storage/src/storage.c`.
- `rewrite_active_update_pages()` rewrites only known row pages and changed
  index-entry pages after validating or reusing the buffered rewrite shape.
- `capture_buffered_page_undo()` only needs the preimage prefix that rollback
  must restore. The active rewrite caller already knows whether it is about to
  rewrite a row page or an index-entry page.

## Design

- Add a typed undo-capture helper that accepts a precomputed meaningful
  `used_size` and reuses the existing duplicate-capture, allocation, checksum
  dirty, and page-copy behavior.
- Keep generic prefix sizing as a conservative fallback inside the helper for
  unusual page types.
- For active row rewrites, compute the old row payload prefix directly from the
  row record size field before mutating the page.
- For changed index-entry rewrites, compute the old key prefix directly from
  the index key-size field before mutating the page.
- Preserve full-page fallback when a decoded size is outside the page format
  capacity.

## Affected Subsystems

- MyLite storage active buffered update rewrite.
- Per-statement savepoint rollback for active append-buffer pages.
- Storage-smoke prepared update performance baseline.

## Compatibility Impact

No SQL, MySQL/MariaDB API, handler, or storage-engine routing behavior changes.
This is a transient storage bookkeeping optimization.

## Single-File And Lifecycle Impact

No durable file-format, journal, lock, recovery, flush, or companion-file
lifecycle change. Statement rollback still restores the same buffered page
preimages.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party C helper split. No new dependency.

## Tests And Verification

- Rebuild `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Run a sampled focused prepared-updates benchmark with macOS `sample` and
  confirm `buffered_page_undo_used_size()` is no longer a visible hot-frame
  child of active buffered update rewrites.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

## Acceptance Criteria

- Active row and index-entry buffered rewrites capture rollback preimages with
  typed prefix sizing.
- Conservative full-page or generic prefix sizing remains available as a
  fallback for unusual page types.
- Existing active update rewrite, savepoint rollback, storage, and embedded
  storage-engine tests remain green.
- Benchmark/profile evidence records the prepared update latency impact.

## Verification Evidence

- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
  1000 1000000` measured one focused run at `4.262 us/op` prepared primary-key
  updates after typed undo sizing.
- A three-second macOS `sample` run over that focused phase no longer showed
  `buffered_page_undo_used_size()` as a visible child of active buffered update
  rewrites. The remaining undo samples were under the typed capture helper and
  direct row/index prefix-size helpers.

## Risks And Open Questions

- The typed path relies on active rewrite validation to guarantee row and index
  page shape before mutation. The implementation must keep full-page fallback
  for out-of-range record sizes so rollback remains conservative if a cached
  shape assumption becomes invalid.
