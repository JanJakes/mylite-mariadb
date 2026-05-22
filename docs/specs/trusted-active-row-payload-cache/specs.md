# Trusted Active Row-Payload Cache Reads

## Problem

After catalog-load deferral, the prepared point-update profile shows
`row_payload_cache_entry_is_valid()` hashing active cached row bytes on every
indexed-row read. Those row bytes are transient statement memory owned by the
active checkpoint, and the update-validation path already accepts active
row-payload cache presence as proof that the row was validated for the same
catalog generation and table.

Durable row-payload caches had a wider lifetime than active caches and kept
their checksum validation in this slice. The later
`trusted-durable-row-payload-cache` slice removes that checksum after making the
durable cache identity and mutation-invalidation boundary explicit.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::index_read_map()` calls
  `mylite_storage_find_indexed_row_reuse()` for exact indexed reads before
  update execution.
- `packages/mylite-storage/src/storage.c::read_indexed_row_payload_from_open_file()`
  checks the active row-payload cache first, then the durable row-payload
  cache, then falls back to row-page reads.
- `packages/mylite-storage/src/storage.c::validate_direct_live_row_in_statement_cache()`
  already accepts the active row-payload cache entry's presence as validation
  proof for the active checkpoint view.
- `packages/mylite-storage/src/storage.c::append_cached_row_payload_to_builder()`
  and durable indexed-row paths still need checksum validation before trusting
  durable cached row bytes.
- A sampled `mylite_perf_baseline --phase=prepared-updates 1000 1000000` run
  after index-root absence caching measured prepared updates at `3.449 us/op`
  and showed `row_payload_cache_entry_is_valid()` under active indexed-row
  materialization as the next visible storage-side cost.

## Design

- Add an active-cache-specific row-payload usability check that only requires a
  present entry and owned row pointer.
- Use that check for the active row-payload cache hit in
  `read_indexed_row_payload_from_open_file()`.
- Stop recomputing row checksums when replacing active row-payload cache
  entries after successful updates.
- Keep durable row-payload cache validation unchanged in this slice.
- Do not change cache invalidation, replacement, rollback, durable cache
  retargeting, or row copy semantics.

## Affected Subsystems

- MyLite storage active row-payload cache.
- Handler-driven indexed-row reads used by routed updates.
- Storage-smoke performance baseline.

## Compatibility Impact

No SQL-visible, handler, public API, or MySQL/MariaDB compatibility behavior
changes. Active cached row bytes are copied to the same output buffer as before.
Durable cache validation remains unchanged.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, lock, journal, recovery, or lifecycle
change. The optimization only changes transient active-cache validation.

## Public API And File-Format Impact

No public `libmylite` API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- Rebuild `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage and embedded storage-engine CTest groups.
- Run the focused prepared-update performance baseline.
- Sample the one-million prepared-update benchmark and confirm active
  row-payload checksum validation moves down or disappears from the indexed-row
  read hot path.
- Run `git diff --check`.
- Run `git clang-format --diff` on the touched C file.

Verification after implementation:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`
- Sampled one-million prepared-update benchmark with macOS `sample`.
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

Measured rerun:

- Prepared primary-key updates: `2.885 us/op`.

Sampled rerun:

- Active indexed-row reads no longer showed row-payload cache checksum
  validation.
- Active row-payload replacement no longer showed row-byte checksum work as a
  meaningful storage-side frame.

## Acceptance Criteria

- Active row-payload cache hits no longer checksum row bytes before copying
  them to indexed-row read output.
- Active row-payload cache replacement no longer computes checksums that active
  readers do not use.
- Durable row-payload cache behavior is unchanged by this slice.
- Existing active row-payload cache rollback, update, delete, and storage smoke
  tests remain green.

## Risks And Open Questions

- This relies on active cache invalidation staying correct. That is consistent
  with the existing update-validation proof, but stale active-cache bugs would
  now affect read output without a second checksum guard.
- This does not reduce the output row copy cost or MariaDB executor overhead.
