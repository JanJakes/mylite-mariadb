# Trusted Durable Row-Payload Cache Hits

## Problem

Prepared primary-key point-select profiling after direct `index_read_idx_map()`
materialization still shows storage-side time in
`row_payload_cache_entry_is_valid()` under
`read_indexed_row_payload_from_open_file()`. That validation recomputes a
checksum over row bytes already copied into the process-local durable
row-payload cache.

The checksum does not validate the durable `.mylite` file. It validates only the
cached heap copy against a checksum computed from that same heap copy when the
entry was stored. Staleness is already guarded by the durable cache identity and
mutation invalidation paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::index_read_idx_map()` reaches
  `read_indexed_row_payload_from_open_file()` through the guarded direct
  exact-unique read path for supported primary-key point selects.
- `packages/mylite-storage/src/storage.c::durable_row_payload_cache_for()`
  resolves durable row-payload caches by filename, catalog root, catalog
  generation, page count, and table id, and disables them while active
  statements or read snapshots own the file.
- `packages/mylite-storage/src/storage.c::retarget_durable_caches_after_table_mutation()`
  clears same-table row-payload caches and retargets other same-file caches
  after durable mutations. Deferred retargeting applies the same rule at
  top-level statement commit.
- `packages/mylite-storage/src/storage.c::row_payload_cache_entry_is_valid()`
  checks only `checksum_bytes(entry->row, entry->row_size) == entry->checksum`.
  That is a process-memory guard, not a file-integrity guard.
- Existing corruption tests still read and verify row pages on uncached paths.
  Once a durable row-payload cache hit is used, current behavior already avoids
  reading the row page again.

## Design

- Treat durable row-payload cache entries as usable when the resolved cache
  identity matches the current durable header view and the entry owns a row
  pointer.
- Use one shared row-payload cache usability helper for active and durable cache
  hits.
- Remove per-entry row checksum storage and recomputation from row-payload
  caches.
- Keep the durable cache key, cache limits, mutation retargeting, snapshot
  exclusion, and copy-out behavior unchanged.
- Keep file page checksums on uncached row-page reads unchanged.

## Affected Subsystems

- First-party MyLite storage row-payload cache.
- Durable exact-index row materialization for routed point reads.
- Indexed-row batch materialization that reuses durable row-payload cache hits.

## Compatibility Impact

No SQL, handler, public C API, metadata, or storage-engine routing behavior
changes. Cached rows are visible only under the same durable header fingerprint
and table identity as before.

## Single-File And Lifecycle Impact

No durable file-format, journal, lock, recovery, or companion-file lifecycle
change. The cache remains transient thread-local process memory and is still
cleared or retargeted by the existing durable mutation paths.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency and no expected binary-size
impact beyond normal code-size noise.

## Tests And Verification

- Reuse existing storage cache, mutation invalidation, corruption, and routed
  storage-engine tests.
- Rebuild `mylite_storage_test`, the storage-smoke MariaDB archive, routed
  smoke tests, and `mylite_perf_baseline`.
- Run focused and full storage-smoke CTest coverage.
- Run prepared primary-key point-select and storage row-lookup performance
  baselines.
- Run `git diff --check` and changed-file formatting checks.

Verification after implementation on 2026-05-22:

- `git diff --check`
- `git-clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --preset dev --output-on-failure -R mylite-storage`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `tools/mylite-perf-baseline --phase=storage-pk-row-lookups 1000 10000`
  measured storage primary-key row lookups at `4.132 us/op`.
- `tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 10000`
  measured prepared primary-key point selects at `7.072 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 1000 1000000` measured prepared primary-key
  point selects at `7.226 us/op`.

## Acceptance Criteria

- Durable row-payload cache hits do not recompute checksums over cached row
  bytes.
- Row-payload cache entries no longer store a private row-byte checksum that no
  file-integrity check consumes.
- Existing row-page checksum validation remains active on uncached reads.
- Mutation and deferred retarget tests continue to prove stale durable
  row-payload caches are not reused after writes.

## Risks And Unresolved Questions

- This relies on existing durable cache identity and invalidation correctness.
  That is the same correctness boundary already required for durable row and
  index caches.
- This does not reduce shared-lock, journal-probe, header-read, or row-copy
  costs. Those remain separate pager and read-scope work.
