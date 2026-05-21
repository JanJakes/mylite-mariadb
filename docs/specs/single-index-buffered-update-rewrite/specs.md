# Single-Index Buffered Update Rewrite

## Problem

Prepared primary-key updates that change one secondary key still spend the
largest first-party storage frame in `rewrite_active_update_pages()`. Earlier
slices optimized row-only cached rewrites and repeated append-buffer lookup,
but the current performance baseline updates `perf_rows.value`, so one indexed
entry changes on every update.

The cached-shape path has already proven the row page, replacement row-state
page, and changed index-page shape for repeated updates to the same row. Even
then, the one-index case still walks the index-entry array once to collect page
refs and again to rewrite the changed page.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `tools/mylite_perf_baseline.c` creates `perf_rows` with a primary key and
  secondary `value_key`, then executes
  `UPDATE perf_rows SET value = value + 1 WHERE id = ?` in a transaction.
- `packages/mylite-storage/src/storage.c::rewrite_active_update_pages()` uses
  `buffered_update_rewrite_shape_known()` to skip buffered row/state/index page
  decode after the shape is cached.
- For `changed_entry_count == 1`, the changed index page id is always
  `row_id + 2`; the active append-buffer range check has already proved that
  page is resident when the rewrite path can proceed.
- The current sampled profile still shows `rewrite_active_update_pages()`,
  buffered undo capture, and `memmove` below the storage update path.

## Design

- Summarize changed index entries once in `rewrite_active_update_pages()`,
  recording the first changed entry index.
- When the buffered shape is cached and exactly one index entry changed:
  - fetch the row page and the single changed index page directly;
  - capture the row-page undo and rewrite the row page;
  - capture and rewrite the index page only when the serialized key bytes
    differ from the existing key bytes;
  - skip the generic changed-index page-ref array and the second changed-index
    loop.
- Keep the existing full validation path for uncached shapes, row-only updates,
  and multi-index changes.

## Scope

In scope:

- First-party active append-buffer rewrite for cached one-index updates.
- Prepared update performance evidence.

Out of scope:

- MariaDB quick-range construction or execution.
- Multi-index update specialization.
- Row-only cached rewrite behavior, already covered by earlier slices.
- File-format, public API, storage routing, transaction, savepoint, or recovery
  behavior changes.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, or file-format behavior
changes. The fast path is selected only after the existing shape cache validates
the table id, changed index number, and key size for the same row id.

## Single-File And Lifecycle Impact

No durable file or companion-file lifecycle change. The change only narrows the
transient in-memory rewrite path before active statement publication.

## Test Plan

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage-smoke CTest coverage.
- Run `git diff --check` and `git clang-format --diff`.
- Run `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`.
- Sample the prepared update phase and compare the storage-side profile shape.

## Acceptance Criteria

- Cached one-index buffered rewrites avoid the generic changed-index page-ref
  array and second changed-index loop.
- Uncached one-index rewrites still validate row, state, and index pages before
  caching shape.
- Multi-index rewrites keep the existing generic path.
- Storage and embedded routed-engine tests pass.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`: no
  changes.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key updates measured
  `2.408 us/op`; the sampled run measured `2.451 us/op`.
- A two-second macOS `sample` run showed the cached one-index path executing in
  `rewrite_active_single_index_update_page()`. The generic
  `rewrite_active_update_pages()` frame moved down, while remaining storage cost
  is primarily row/index page copy, buffered undo capture, table-id lookup, and
  buffered rewrite shape lookup.
