# Indexed Rowset Builder

## Problem

Secondary-index exact reads materialize a known batch of row ids into a
`mylite_storage_rowset`. The current append helper grows the row byte buffer
and all metadata arrays with `realloc()` for every row. Sampling after active
checkpoint write-path work showed allocator frames under
`append_row_to_rowset()` while materializing repeated indexed-row batches.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::materialize_index_cursor_rows()` calls
  `mylite_storage_read_indexed_rows()` with the cursor's known row-id batch.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_indexed_rows()`
  appends one row per input row id, often through the durable row-payload cache.
- `append_row_to_rowset()` preserves the public rowset layout but reallocates
  `rows`, `row_offsets`, `row_sizes`, and `row_ids` on every append.
- The public `mylite_storage_rowset` layout has no capacity fields, so capacity
  tracking must remain internal to a builder used during construction.

## Design

- Add an internal rowset builder that references the public rowset plus
  transient row-byte and metadata capacities.
- Preallocate row offsets, sizes, and ids for the known `row_id_count` in
  `mylite_storage_read_indexed_rows()`.
- Append cached and newly-read indexed rows through the builder.
- Grow the row byte buffer amortized. When the first row arrives and its size
  can be multiplied by the known metadata capacity, reserve that expected byte
  count up front.
- Keep `mylite_storage_rowset` unchanged and keep the existing
  `append_row_to_rowset()` helper for callers that do not know row counts.

## Compatibility Impact

SQL-visible behavior is unchanged. Row order, row ids, offsets, row sizes,
`row_size` fixed-width reporting, and cleanup through
`mylite_storage_free_rowset()` remain the same.

## Single-File And Lifecycle Impact

No file-format change and no new companion files. The builder is transient
process memory used only while constructing a storage API result.

## Test And Verification Plan

- Existing storage and storage-engine tests cover indexed-row materialization,
  rowset cleanup, secondary reads, and cached durable row payloads.
- Rebuild and run the storage unit tests.
- Rebuild the storage-smoke target and run the storage-engine compatibility
  harness.
- Run the local performance baseline to measure secondary exact-select impact.
- Run changed-line formatting checks and `git diff --check`.

## Acceptance Criteria

- Indexed batch row materialization avoids per-row metadata reallocations.
- Row byte storage grows amortized rather than once per appended row.
- Storage and storage-engine compatibility tests pass.
- Secondary exact-select timings improve or stay within noise without changing
  result checksums.

## Risks

- This does not change handler cursor construction, SQL sorting, durable page
  reads, or the row-payload cache lookup cost. It only removes avoidable
  allocation churn while building rowset results.
