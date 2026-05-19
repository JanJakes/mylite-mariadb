# Index Cursor Row Materialization

## Problem

The local performance sample after exact-index cache maintenance showed
secondary exact-select time dominated by repeated row payload reads from
`ha_mylite::read_index_cursor_row()`. Each row from a durable index cursor called
`mylite_storage_read_indexed_row()`, reopening the primary `.mylite` file and
revalidating header/catalog state for every row returned by the cursor.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` already
  materializes durable index entries into handler-owned cursor state.
- `mariadb/storage/mylite/ha_mylite.cc::read_index_cursor_row()` then read each
  indexed row payload separately.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_indexed_row()`
  delegates to `read_row_payload()` and opens the primary file per call when no
  write checkpoint is active.

## Design

- Add `mylite_storage_read_indexed_rows()` to read an ordered list of row ids
  into a `mylite_storage_rowset` using one file open, header read, and catalog
  lookup.
- Keep the existing single-row API for callers that need one payload.
- Have durable handler index cursors materialize row payloads after sorting the
  cursor row ids. `read_index_cursor_row()` can then decode the payload from
  cursor-owned memory without another storage call.
- Leave volatile table reads on the existing per-row path because they do not
  pay file-open or catalog-checksum costs.

## Compatibility Impact

SQL-visible results are unchanged. The cursor reads the same row ids already
returned by the storage index-entry APIs and preserves their sorted order.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The change only reduces
transient handler/storage round trips during a single cursor lifetime.

## Test And Verification Plan

- Add storage unit coverage for ordered indexed-row batch reads.
- Rebuild storage and storage-smoke targets.
- Run storage unit tests and the storage-engine compatibility harness.
- Run the local performance baseline to measure secondary exact-select impact.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Batch indexed-row reads preserve requested row-id order and payload contents.
- Durable index cursors avoid per-row storage opens after cursor build.
- Existing index, update/delete, rollback, and routed storage tests pass.
- Secondary exact-select timings improve materially in the local benchmark.

## Risks

- This increases transient cursor memory proportional to matched row payloads.
  The current append-only index implementation already materializes matching
  index entries, so this is acceptable until maintained navigable indexes remove
  the broad cursor materialization cost.
- Large-result secondary scans still need real pager and cursor work; this is a
  targeted reduction of repeated file/open/catalog overhead, not the final
  SQLite-like access path.
