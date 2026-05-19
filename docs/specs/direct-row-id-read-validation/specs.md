# Direct Row-Id Read Validation

## Problem

Index cursors lazily materialize selected row ids through
`mylite_storage_read_row()`. That function rebuilt the full row-state map for
each selected row, so exact secondary reads that returned many row ids paid a
file-wide row-state scan repeatedly after the index lookup had already filtered
stale entries.

## Design

- Keep the public `mylite_storage_read_row()` API unchanged.
- Validate the requested row id against the current header bounds before
  reading the page, preserving `MYLITE_STORAGE_NOTFOUND` for rolled-back row ids
  beyond the restored file tail.
- Read the target row page directly by page id and verify the owning table id.
- Check liveness by scanning only later row-state pages for a matching
  `source_row_id`. Row-state pages are append-only and cannot hide a row before
  the source row page exists.
- Keep full row-state-map construction for table scans, row counts, mutations,
  and exact index scans where all live/stale entries are being classified.

## Compatibility Impact

No SQL-visible behavior change. Hidden, deleted, superseded, rolled-back, and
wrong-table row ids still return not found, and corrupt later row-state pages
still fail the read.

## Single-File And Lifecycle Impact

No file-format or lifecycle change. The optimization relies on the existing
append-only row-state invariant inside the primary `.mylite` file.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

MyLite handler index cursors benefit because selected durable row ids can be
materialized without rebuilding the complete row-state map for each row.

## Tests And Verification

- Existing row lifecycle and statement-checkpoint tests cover hidden rows,
  rolled-back row ids, and direct row-id reads.
- Run storage unit tests, storage-engine compatibility harness, performance
  baseline, formatting check, and whitespace check.

## Local Verification

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `tools/mylite-compat-harness run storage-engine`
- `build/storage-smoke-dev/tools/mylite_perf_baseline 1000 1`
  - Direct secondary exact read: `125018 us/op`.
  - Prepared secondary exact read: `124510 us/op`.
  - Direct published-leaf secondary exact read: `132857 us/op`.
  - Prepared published-leaf secondary exact read: `135257 us/op`.

## Acceptance Criteria

- `mylite_storage_read_row()` does not build the full row-state map for ordinary
  direct row-id materialization.
- Row lifecycle and statement rollback behavior remains unchanged.
- Published-leaf secondary exact reads no longer spend most of their time in
  repeated row-state map construction.

## Risks

- The direct liveness check depends on row-state pages remaining append-only.
  Future free-space reuse or in-place state pages must revisit this read path.
