# Buffered Rewrite Page Refresh

## Problem

The direct buffered update rewrite path mutates still-unpublished row and
changed index-entry pages in the active append buffer. The implementation still
uses the normal fresh-page encoders for those mutations. Fresh encoders are
correct for newly allocated append pages, but they clear and rebuild the whole
4096-byte page image even though a buffered rewrite has already validated that
the page id, table id, page type, row id, and fixed metadata belong to the
replacement run being rewritten.

After skipping unchanged unique duplicate checks, the sampled update profile
still showed `checksum_page_zero_tail()` and `capture_buffered_page_undo()` as
the dominant storage-side costs. Avoiding full page rebuilds does not remove
checksum work or rollback preimage copies, but it removes avoidable hot-loop
`memset()` and fixed metadata stores before the checksum.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::rewrite_active_update_pages()`
  validates the current buffered row page, the matching row-state page, and
  changed index-entry pages before mutation.
- `encode_row_page()` and `encode_index_entry_page()` are fresh-page encoders:
  they clear the whole page, write fixed metadata, write payload/key bytes, and
  compute the checksum.
- Buffered rewrite pages are unpublished in-process pages that were created by
  those fresh encoders earlier in the same active checkpoint. The rewrite path
  has already verified page type, page id, table id, row id, key size, and
  replacement-row linkage.
- The existing active rewrite storage test covers growing and shrinking row
  payloads across nested savepoint rollback.

## Design

- Keep fresh page encoders unchanged for newly appended row and index pages.
- Add rewrite-only row and index page refresh helpers for validated buffered
  pages.
- For row pages, update the record size, reset the overflow root to zero,
  overwrite the row payload, zero bytes that were used by the old payload but
  are no longer used by a shorter new payload, and recompute the page checksum.
- For index-entry pages, update the index number and key size, overwrite key
  bytes, zero bytes that were used by an older longer key, and recompute the
  page checksum.
- Keep full-page undo capture before mutation. Rollback still restores the
  exact prior buffered page image.
- Keep durable read and decode paths unchanged. Persisted pages still validate
  with the full-page checksum stream.

## Affected Subsystems

- MyLite active append-page buffer.
- Active update rewrite page mutation.
- Statement rollback over nested row-DML checkpoints.
- Storage-smoke update performance baseline.

## Compatibility Impact

No SQL, handler, public API, or MySQL/MariaDB compatibility behavior changes.
The optimized path rewrites the same logical row and index-entry pages with the
same page format and checksum algorithm.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, or lifecycle change. The refresh helpers
only operate on transient unpublished pages already resident in the active
append buffer.

## Public API And File-Format Impact

No public API or file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- Run the storage unit test and full storage-smoke CTest gate.
- Run the update performance baseline with `--phase=updates 1000 1000000`.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

Verification results:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 1000000`
  first produced a clear outlier at `20.080 us/op` direct and `7.967 us/op`
  prepared primary-key updates; the immediate rerun reported direct primary-key
  updates at `12.801 us/op` and prepared primary-key updates at `5.909 us/op`.
- A sampled one-million-update run reported direct primary-key updates at
  `13.142 us/op` and prepared primary-key updates at `5.893 us/op`.
  The rewrite path now appears under `rewrite_buffered_row_page()` and
  `rewrite_buffered_index_entry_page()`, with checksum work and undo capture
  still visible as expected.
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

## Acceptance Criteria

- Repeated buffered update rewrites keep row-id reuse and savepoint rollback
  behavior.
- Shrinking row payloads and index keys zero stale bytes that were inside the
  old used region but are outside the new used prefix.
- Existing storage and embedded storage-engine tests remain green.
- The update baseline remains correct and shows no regression from replacing
  full fresh-page rebuilds with targeted in-buffer refreshes.

## Risks And Open Questions

- This still recomputes the page checksum after every mutation. SQLite-like
  write throughput still needs a pager/WAL design or an equivalent checksum
  strategy that preserves corruption detection.
- Full-page undo preimage copies remain the next major storage hot spot for
  nested statement rollback.
