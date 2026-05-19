# Coalesced Inline Update Pages

## Problem

The local prepared-update sample on 2026-05-19 showed
`mylite_storage_update_row_with_index_entries()` dominated by repeated page
append writes. The common small-row update path writes one row page, one
row-state page, and one page per replacement index entry, even though those
pages are contiguous in the primary `.mylite` file.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::update_row()` serializes the new row
  payload and replacement index entries, then calls
  `mylite_storage_update_row_with_index_entries()`.
- `packages/mylite-storage/src/storage.c::mylite_storage_update_row_with_index_entries()`
  currently calls `write_row_payload_pages()`, writes a row-state page, and then
  calls `write_index_entry_pages()`.
- `write_row_payload_pages()` stores small rows inline at `header.page_count`,
  and the update path writes the row-state page and index-entry pages
  immediately after it.

## Design

- Add an inline update append fast path for rows that fit in one row page and
  files using the current fixed page size.
- Encode the replacement row page, row-state page, and replacement index-entry
  pages into one contiguous temporary page buffer.
- Write the contiguous page run with one offset-addressed file write.
- Preserve the existing overflow-row and unusual-page-size paths by falling
  back to the current per-page writers.
- Keep page ids, row ids, row-state semantics, checksums, and index-entry
  layout unchanged.

## Compatibility Impact

SQL-visible behavior is unchanged. The same durable pages are appended in the
same order; only the write syscall shape changes for the common inline update
case.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The existing recovery journal
and transaction rollback model still protects the same header/catalog state and
append tail.

## Public API And File-Format Impact

No public API change and no `.mylite` format change.

## Storage-Engine Routing Impact

Routed `UPDATE` statements over MyLite storage benefit when the replacement row
is inline. BLOB/TEXT overflow payload updates keep the existing path until the
blob writer has its own batching design.

## Test And Verification Plan

- Use storage unit coverage that exercises active transaction updates with
  replacement index entries.
- Build storage-smoke targets.
- Run the full `storage-smoke-dev` CTest set.
- Run the local performance baseline with default and higher iteration counts.
- Run `git diff --check`.

## Acceptance Criteria

- Inline update appends write the same row, row-state, and index-entry pages as
  before, with unchanged row ids and visibility semantics.
- Overflow-row updates continue through the existing writer path.
- Existing storage and routed-engine tests pass.
- The local performance baseline shows lower direct and prepared update times.

## Risks

- The fast path allocates a temporary buffer proportional to replacement index
  count. It must reject overflow and fall back or fail before allocation size
  wraps.
- Future pager work may replace this with a broader page-cache/WAL design; this
  slice is a narrow improvement that keeps the current append-only format.
