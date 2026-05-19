# Indexed Row Payload Cache Probe

## Problem

Fresh local sampling of the routed performance baseline shows secondary exact
reads spending visible time inside row-payload cache availability and lookup
helpers while materializing indexed row-id batches. The durable row-payload
cache already prevents repeated row-page reads after the first batch, but the
batch loop still rechecks cache availability and active-cache state for every
row id.

That repeated control-plane work is small per row, but it is paid millions of
times in many-match secondary-index workloads.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::build_index_cursor()` gathers matching
  row ids from MyLite storage and then calls
  `materialize_index_cursor_rows()` for the selected row batch.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_indexed_rows()`
  materializes those row ids and probes active and durable row-payload caches
  before falling back to row-page reads.
- `packages/mylite-storage/src/storage.c::read_row_ids_into_rowset()` performs
  the same durable-cache materialization for published-leaf index runs.
- `packages/mylite-storage/src/storage.c::durable_row_payload_cache_for_batch()`
  accepts a caller-held cache pointer and generation, but still checks
  durable-cache availability before reusing the already-resolved pointer.
- The 2026-05-20 local sample of `mylite_perf_baseline 1000 300000` showed
  `find_row_payload_cache_entry()`,
  `durable_row_payload_cache_available()`, and active-cache probes under
  `mylite_storage_read_indexed_rows()` during secondary exact reads.

## Design

Keep the row-payload cache format and visibility rules unchanged.

- Resolve active row-payload cache availability once per indexed-row batch.
- Skip active-cache probes entirely when no active checkpoint can own such a
  cache for the file.
- Let durable batch cache lookup reuse the caller-held cache pointer whenever
  the durable cache-set generation is unchanged, including the known-empty
  case before the first row-page miss populates the cache.
- Continue to refresh the durable cache pointer after a miss stores a newly
  read row payload, preserving existing cache-limit and generation handling.

## Compatibility Impact

No SQL or API behavior changes. MyLite still materializes the same row ids in
the same order and validates the same row pages on cache misses.

## Single-File And Lifecycle Impact

No file-format, journal, lock, or companion-file change.

## Public API And File-Format Impact

No public API or durable file-format change.

## Storage-Engine Routing Impact

All durable routed engines using MyLite indexed reads benefit, including
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted/default engines, and
`ENGINE=MYLITE`.

## Binary-Size And Dependency Impact

Small first-party C code only. No new dependency.

## Tests And Verification

- Reuse existing indexed-row batch cache coverage.
- Run storage unit tests and routed storage-engine tests.
- Run the local performance baseline to compare secondary exact-read timings.
- Run `git diff --check` and formatting checks.

## Acceptance Criteria

- Indexed row materialization still returns the expected rows before and after
  cache hits.
- Durable row-payload cache pointers are reused within a stable cache-set
  generation.
- Existing storage and storage-smoke tests pass.
- The local benchmark does not regress secondary exact-read timings.

## Risks And Open Questions

- This does not reduce SQL optimizer, filesort, or MariaDB handler overhead in
  secondary exact reads.
- The first uncached materialization still reads row pages and copies payloads;
  maintained navigable indexes and a broader row materialization contract
  remain future work.
