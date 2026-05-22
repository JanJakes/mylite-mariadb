# Durable Cache Filename Identity

## Problem

Prepared primary-key point selects now reuse the active read scope and direct
exact-index handler path, but the first-row component still costs roughly
`2.1 us/op` locally. Sampling the opt-in storage-smoke benchmark shows the hot
row path remains dominated by MariaDB prepared execution, with a smaller but
measurable storage slice inside exact-index and row-payload cache lookup.

The storage cache slice still falls back to filename `strcmp()` checks even
when the handler has opened a trusted filename identity scope around the storage
call. Live-row-id caches already exploit that identity; durable exact-index and
row-payload caches do not.

## Source Findings

- Base source is MariaDB 11.8.6, initial import
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/libmysqld/lib_sql.cc` routes embedded `mysql_stmt_execute()` through
  `emb_stmt_execute()` and `emb_advanced_command()`, then
  `dispatch_command(COM_STMT_EXECUTE)`.
- `mariadb/sql/sql_prepare.cc` runs `mysql_stmt_execute_common()`,
  `Prepared_statement::execute_loop()`, and `Prepared_statement::execute()`
  before `mysql_execute_command()`.
- `mariadb/sql/sql_select.cc` const-table optimization reaches
  `join_read_const_table()` / `join_read_const()`.
- `mariadb/storage/mylite/ha_mylite.cc` routes supported const-table reads
  through `ha_mylite::index_read_idx_map()` and
  `ha_mylite::read_exact_unique_index_row_into()`, which wrap the storage call
  in `Mylite_filename_identity_scope`.
- `packages/mylite-storage/src/storage.c` has trusted filename identity support
  for statements and live-row-id caches:
  `active_statement_filename_matches()`,
  `store_active_statement_filename_identity()`,
  `live_row_id_cache_filename_matches()`, and
  `store_live_row_id_cache_filename_identity()`.
- The same file still uses direct `strcmp()` matching in
  `durable_exact_index_cache_matches()` and in durable/active row-payload cache
  lookup.

Local sample evidence for
`./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 1000 1000000`
shows `ha_mylite::read_exact_unique_index_row_into()` below MariaDB's const-table
read, with samples in `find_cached_durable_exact_index_entry()`,
`durable_exact_index_cache_for()`, `find_durable_exact_index_cache()`,
`durable_exact_index_cache_matches()`, `durable_row_payload_cache_for()`, and
`find_durable_row_payload_cache()`.

## Design

Add the existing filename identity shortcut to durable exact-index and
row-payload cache records:

- Store the active identity pointer on newly appended exact-index and
  row-payload caches when the active identity scope proves the caller's filename
  pointer is stable.
- Match by the stored identity pointer before falling back to pointer equality
  or `strcmp()`.
- Preserve the existing value-comparison fallback so unscoped callers and
  mutable filename buffers remain correct.
- Keep cache invalidation, retargeting, and file lifecycle unchanged.

## Compatibility Impact

This is an internal storage lookup optimization. It does not change SQL
behavior, `libmylite` API behavior, metadata routing, engine routing, result
types, diagnostics, or file format.

## Single-File And Lifecycle Impact

No new files, durable companions, locks, journals, or recovery behavior are
introduced. The cache identity is thread-local process memory and is discarded
with the existing cache record.

## Binary Size And Dependency Impact

No new dependencies. Binary-size impact should be negligible: a few pointer and
flag fields plus small helper functions.

## Test And Verification Plan

- Add storage test-hook coverage proving exact-index and row-payload durable
  caches record a trusted filename identity only inside
  `mylite_storage_begin_filename_identity_scope()`.
- Run `git diff --check`.
- Build `mylite_storage_test` and `mylite_perf_baseline` with
  `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_perf_baseline`.
- Run `ctest --preset storage-smoke-dev -R mylite-storage.capabilities`.
- Run focused benchmarks:
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 1000 100000`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-select-components 1000 100000`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-row-lookups-one-read 1000 10000`

## Acceptance Criteria

- Durable exact-index and row-payload caches use trusted filename identity when
  available.
- Existing filename-value fallback remains intact.
- Storage capability tests pass.
- Prepared point-select and storage point-read benchmarks remain correct and do
  not regress materially on the local machine.

## Risks And Open Questions

- This only addresses a small storage-side cost. The larger remaining prepared
  point-select cost is still MariaDB prepared SELECT execution and optimizer
  work.
- Benchmarks are local and noisy; treat the change as a targeted cache cleanup
  unless repeated samples show a stable throughput gain.
