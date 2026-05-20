# Large Append Buffer Flush Window

## Problem

The active append-page buffer currently keeps up to 4096 pages, or 16 MiB, of
unpublished replacement pages in memory. That covers the 1000-row prepared
update benchmark, but a 10,000-row prepared update loop still flushes before
the first replacement generation is complete. A local sample on 2026-05-20
showed `pwrite()` under `flush_statement_append_page_buffer()` dominating that
larger loop.

MyLite still needs a real pager/WAL design, but the current append-buffer
architecture can keep the larger repeated-update loop on the in-memory rewrite
path by widening the transient window.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`) for the embedded transaction
  and handler statement flow.
- `packages/mylite-storage/src/storage.c::buffer_append_pages_at_raw()` flushes
  when appending would exceed `MYLITE_STORAGE_APPEND_PAGE_BUFFER_LIMIT_PAGES`.
- `rewrite_active_update_pages()` can rewrite transaction-local replacement
  pages only while the row, row-state, and changed index-entry pages are still
  resident in the append buffer.
- The current 10,000-row prepared-update sample measured
  `26.219 us/op` and sampled mostly in `pwrite()` from append-buffer flushes.

## Design

- Increase `MYLITE_STORAGE_APPEND_PAGE_BUFFER_LIMIT_PAGES` from 4096 to 32768.
- Keep the existing geometric allocation and flush lifecycle unchanged.
- Accept the larger transient worst case: 32768 pages at 4096 bytes per page is
  128 MiB per active top-level checkpoint.
- Update storage architecture documentation to record the new memory window.

## Affected Subsystems

- First-party MyLite storage active checkpoint append buffering.
- Large transaction row-DML update performance.

## Compatibility Impact

No SQL-visible behavior change. This only changes how long unpublished
append-only pages stay in process memory before the existing flush path writes
them to the primary file.

## Single-File And Lifecycle Impact

No durable file-format or companion-file change. The buffer is transient
process memory. Top-level commit still flushes before header publication, and
rollback still flushes retained prefixes before truncating discarded tails.

## Binary-Size Impact

Constant-only first-party change. No new dependency.

## Tests And Verification

- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 10000 1000000`

## Acceptance Criteria

- The 10,000-row prepared-update loop shows the expected drop from the
  pwrite-heavy 4096-page-window baseline.
- Existing append-buffer savepoint rollback coverage remains green.
- The storage architecture document states the new memory tradeoff.

## Verification Results

- `git diff --check`
- `git clang-format --diff`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --preset storage-smoke-dev --output-on-failure`: 10/10 tests passed.
- Before this slice, `tools/mylite-perf-baseline --phase=prepared-updates 10000 1000000`
  measured prepared primary-key updates at `26.219 us/op`, checksum
  `51138894`.
- With the 32768-page window, the same command measured `9.623 us/op` and then
  `9.741 us/op`, checksum `51138894`.
- `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`
  measured `3.205 us/op`, checksum `1512893`.

## Risks And Unresolved Questions

- This trades memory for fewer flushes. It is appropriate for the current
  performance-focused profile, but a future constrained-memory profile should
  expose a smaller configured window.
- Larger update sets can still outgrow the window. SQLite-like sustained update
  performance still needs the planned pager/WAL and navigable index work.
