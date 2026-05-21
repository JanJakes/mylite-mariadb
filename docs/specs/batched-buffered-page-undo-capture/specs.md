# Batched Buffered Page Undo Capture

## Problem

Cached active updates with one changed index entry rewrite two unpublished
append-buffer pages: the row page and the changed index-entry page. Each nested
statement must capture both preimages before mutation so statement rollback can
restore the parent transaction's buffered pages.

The current hot path calls the single-page undo capture helper twice. The first
call appends the row preimage; the second repeats duplicate-detection and small
list setup before appending the index preimage, even though the hot cached path
knows the two page ids are distinct and the nested statement's undo list is
normally empty.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite owns the active checkpoint, append-buffer, and rollback preimage
  implementation in `packages/mylite-storage/src/storage.c`.
- `rewrite_active_update_pages()` dispatches the cached one-index shape to
  `rewrite_active_single_index_update_page()` after the append-buffer range and
  cached row/index shape have been validated.
- `capture_buffered_page_undo_from_page()` already skips pages that did not
  exist at the statement checkpoint, deduplicates page ids, stores the checksum
  dirty bit, and copies only the meaningful preimage prefix.
- `mylite_storage_rollback_statement()` restores buffered-page undo entries
  before trimming the active append buffer, so the batched helper must produce
  the same undo entries in the same order.

## Design

- Add a two-page undo capture helper for the cached single-index rewrite path.
- Use the helper only for the empty-undo-list fast path with distinct page ids.
  Fall back to two ordinary single-page captures when an earlier preimage
  exists, a bucketed undo list is present, or both requested pages resolve to
  the same page id.
- Preserve the existing per-page rules:
  - page ids beyond the statement-start page count do not need an undo entry,
  - missing page bytes are corrupt only when the page needs capture,
  - used-size `0` still falls back to generic page-prefix sizing,
  - out-of-range typed used sizes are clamped to the full page.
- Keep rollback order as row preimage first, then index preimage.

## Affected Subsystems

- MyLite storage active buffered update rewrites.
- Per-statement savepoint rollback for active append-buffer pages.
- Storage-smoke prepared update performance baseline.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, or MariaDB behavior
change. This is transient rollback bookkeeping under the MyLite storage layer.

## Single-File And Lifecycle Impact

No durable file-format, journal, lock, recovery, flush, or companion-file
lifecycle change. Statement rollback restores the same buffered pages and the
same checksum-dirty state as before.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party C helper. No new dependency and no expected meaningful binary
size impact.

## Tests And Verification

- Rebuild `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample a focused prepared-update benchmark and check whether the storage
  rewrite stack moved away from duplicate single-page undo setup.
- Run `git diff --check` and `git clang-format --diff` on the touched C file.

## Acceptance Criteria

- Cached single-index active rewrites capture row and index rollback preimages
  through one batched helper when the statement undo list is empty.
- Non-empty, bucketed, same-page, or unusual capture cases keep using the
  existing single-page helper.
- Existing active rewrite, savepoint rollback, transaction rollback, storage,
  and embedded storage-engine tests pass.
- Benchmark/profile evidence records the prepared-update latency impact.

## Verification Evidence

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: 2/2
  passed.
- `ctest --preset storage-smoke-dev --output-on-failure`: 10/10 passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key updates measured
  `2.284 us/op` in a focused local run after the change. A second sampled run
  measured `2.402 us/op`; treat these as local machine evidence, not portable
  thresholds.
- A one-second macOS `sample` run over the focused prepared-update benchmark
  showed the cached single-index rewrite using
  `capture_buffered_page_undo_pair_from_pages()` and did not show
  `capture_buffered_page_undo_from_page()` in that sampled hot path.
- `git diff --check`: passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`: no
  changes.

## Risks And Open Questions

- The optimization is intentionally limited to the empty-list hot path. Broader
  batching across arbitrary changed-index counts could save more work, but would
  also complicate duplicate handling and rollback ordering for uncommon shapes.
