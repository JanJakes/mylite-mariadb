# Lazy Buffered Page Checksums

## Problem

Repeated active updates now rewrite unpublished row and changed index-entry
pages in the append-page buffer. The current implementation recomputes the FNV
page checksum immediately after each in-memory rewrite, even though those pages
are not durable until the buffer is flushed and the active header is published.

The update benchmark repeatedly mutates the same bounded set of row and
secondary-index pages inside one transaction. After direct buffered rewrites,
duplicate-check elision, targeted page refresh, and compact undo, profiles show
`checksum_page_zero_tail()` as the primary remaining storage cost.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite-owned append buffering is implemented in
  `packages/mylite-storage/src/storage.c`.
- `rewrite_active_update_pages()` mutates only unpublished pages that are still
  resident in the active append buffer.
- `decode_buffered_row_page_metadata()` and
  `decode_buffered_index_entry_page()` intentionally validate buffered rewrite
  candidates without a full checksum scan.
- Generic `read_page_at()` can still copy buffered pages to callers that then
  use durable decode paths with full checksum validation.
- `flush_statement_append_page_buffer()` writes the buffered page bytes to the
  primary file before top-level header publication, so it is the last required
  point to make checksums durable-correct.

## Design

- Add a per-page dirty-checksum bitmap to the active append-page buffer.
- Keep newly appended pages checksum-clean because fresh encoders already write
  valid checksums.
- Let buffered row/index rewrite helpers mutate bytes and mark those page
  checksums dirty instead of recomputing immediately.
- Preserve savepoint rollback by recording each undo preimage's dirty-checksum
  state and restoring it with the page bytes.
- When a generic buffered-page read copies a dirty page, refresh that page's
  checksum first and clear its dirty mark.
- Before flushing buffered pages to the primary file, refresh every dirty page
  and clear the dirty marks.
- Limit dirty checksum refresh to row and table-index entry pages because those
  are the only page types this slice marks dirty.

## Affected Subsystems

- Active append-page buffering.
- Buffered update rewrite.
- Savepoint rollback over buffered page preimages.
- Generic buffered-page reads.
- Storage-smoke update performance baseline.

## Compatibility Impact

No SQL, public API, handler API, storage-engine routing, or MySQL/MariaDB
compatibility behavior changes. Durable pages keep the same FNV checksum
algorithm and stored checksum values as immediate checksum refresh.

## Single-File And Lifecycle Impact

No durable file-format or companion-file change. Dirty checksum state is
transient process memory attached to active append buffers and undo entries.
Dirty pages are refreshed before they can leave the buffer for a generic
checksum-validating read or a durable flush.

## Public API And File-Format Impact

No public API or file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`
- sampled benchmark run with macOS `sample`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

Measured update baseline after the final full CTest pass:

- Direct primary-key updates: `11.715 us/op`.
- Prepared primary-key updates: `5.101 us/op`.

Sampled rerun:

- Direct primary-key updates: `11.674 us/op`.
- Prepared primary-key updates: `5.086 us/op`.

The sampled profile no longer showed `checksum_page_zero_tail()` as the
dominant repeated-update cost. Storage-side samples moved mostly into handler
update and index-cursor work, with `capture_buffered_page_undo()` still visible
at a much lower level.

One full CTest rerun after adding defensive dirty-side-table guards failed in
`libmylite.embedded-storage-engine` with `wp_options` reported as crashed
during the WordPress installer seed assertion. The retained `.mylite` file's
page checksums scanned clean. A temporary diagnostic build printed no rejected
dirty page and passed the isolated embedded-storage-engine CTest; after removing
the diagnostic, the same implementation passed the isolated test again and then
the full storage-smoke CTest gate.

## Acceptance Criteria

- Repeated active updates keep row and index-entry page checksums dirty until a
  generic read or buffer flush needs a valid checksum.
- Generic reads of dirty buffered pages still pass ordinary durable checksum
  validation.
- Savepoint rollback restores both the page preimage and its dirty-checksum
  state.
- Flushed buffered pages are durable-correct with no file-format change.
- Existing storage and embedded storage-engine tests remain green.
- Update profiles show immediate row/index rewrite checksum recomputation no
  longer dominating the repeated-update path.

## Risks And Open Questions

- This reduces repeated in-memory checksum work, but final flush still pays the
  checksum cost once per dirty row/index page.
- The slice intentionally does not change full durable decode validation, which
  remains the corruption-detection boundary for primary-file reads.
- A future pager or WAL design may replace this append-buffer-specific dirty
  tracking with a broader dirty-page model.
