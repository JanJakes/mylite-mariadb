# Unchanged Row-Id Cache Maintenance Skip

## Problem

Repeated active buffered updates can rewrite the same unpublished row page in
place. When the row id is reused, active exact-index cache entries whose key
image did not change are already correct. The current post-update maintenance
still removes and re-adds exact-index cache entries for every cached index, and
live-row caches remove and re-add the same row id.

That redundant cache maintenance is visible in the focused prepared update
profile under `replace_active_exact_index_cache_entries()` and exact-index cache
compaction helpers.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `update_row_with_index_entries()` sets `position.row_page_id = row_id` when
  `rewrite_active_update_pages()` rewrites the active append-buffer row in
  place.
- `index_entry_changed` marks each MariaDB key image whose old and new
  serialized bytes differ.
- After those in-place updates, `replace_active_exact_index_cache_entries()`,
  `replace_active_live_row()`, and `replace_active_live_row_id()` still perform
  remove/add work for unchanged key images and unchanged row ids.

## Design

- Detect successful updates where `position.row_page_id == row_id`.
- For active exact-index caches, skip a cached index only when every matching
  `index_entry_changed` slot says that key image is unchanged.
- Skip active live-row and live-row-id replacement when the row id remains the
  same because the row remains live under that id.
- Keep row-payload replacement because the row bytes changed.
- Keep durable cache retargeting and all ordinary append-only update maintenance
  unchanged.

## Affected Subsystems

- MyLite storage active update cache maintenance.
- Storage-smoke prepared update performance baseline.

## Compatibility Impact

No SQL or MySQL/MariaDB compatibility behavior changes. This is a transient
cache-maintenance optimization after successful storage writes.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, lock, transaction, journal, recovery, or
flush lifecycle change.

## Public API And File-Format Impact

No public `libmylite` API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party C guard. No new dependency.

## Tests And Verification

- Rebuild `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Run a sampled focused prepared-updates benchmark with macOS `sample` and
  confirm exact-index cache replacement frames move down for unchanged-key
  in-place updates.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

## Acceptance Criteria

- In-place active rewrites do not remove/reinsert exact-index cache entries
  when all matching index images are unchanged or live-row cache entries for
  the same row id.
- Updates that append a new row id, delete a row, or change any index entry keep
  the existing cache maintenance behavior.
- Existing storage and embedded storage-engine tests remain green.
- Benchmark/profile evidence records the prepared update latency impact.

## Verification Evidence

- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
  1000 1000000` measured one focused run at `4.567 us/op` prepared primary-key
  updates after the skip.
- A three-second macOS `sample` run over that focused phase showed the next
  storage hotspot under buffered update rewriting and undo capture. Remaining
  exact-index cache maintenance samples were small and expected for the changed
  secondary key image; live-row-id retargeting was not visible in the sampled
  hot path.

## Risks And Open Questions

- This assumes the changed-index vector is authoritative for active exact-index
  cache key images. That is already the basis for unchanged-key duplicate-check
  elision and buffered update rewrite shape validation.
