# Direct Update Trusted Exact Read

## Problem

Accepted MyLite direct updates already prove their exact unique-key shape before
`ha_mylite::direct_update_rows()` executes. The internal row read still calls
the generic `read_exact_unique_index_row_into()` path, which re-checks the key
shape with `mylite_key_is_supported()` and
`mylite_key_uses_raw_exact_filter()` before reading the row.

A delayed local sample of
`build/storage-smoke-dev/tools/mylite_perf_baseline
--phase=prepared-update-components 10000 1000000` still shows steady-loop
samples in:

- `ha_mylite::read_exact_unique_index_row_into()`
- `mylite_key_uses_raw_exact_filter()`
- `mylite_direct_update_key_is_supported()`
- `mylite_storage_find_indexed_row_into()`

The storage lookup is real work, but the repeated eligibility checks are
redundant for an accepted direct-update statement.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `ha_mylite::info_push(INFO_KIND_MYLITE_UPDATE_EXACT_KEY)` accepts a SQL-layer
  exact-key proof only after `mylite_direct_update_key_is_supported()` accepts
  the target key.
- `ha_mylite::direct_update_rows_init()` rejects unsupported table shapes,
  volatile rows, BLOB/TEXT row materialization, unsafe key-changing updates, and
  FK-sensitive key changes before direct execution can run.
- `ha_mylite::build_direct_update_key()` materializes a full key image into the
  cached handler key buffer before the row read.
- `ha_mylite::direct_update_rows()` calls
  `read_exact_unique_index_row_into()` only after those gates pass.
- Generic `index_read_map()` and `index_read_idx_map()` also call
  `read_exact_unique_index_row_into()` and still need the defensive eligibility
  checks because they serve normal handler reads.

## Design

- Add an internal trusted flag to `read_exact_unique_index_row_into()`.
- Keep normal handler index reads on the current defensive path.
- Have `direct_update_rows()` pass the trusted flag after
  `direct_update_rows_init()` and `build_direct_update_key()` have accepted the
  statement and key image.
- In trusted mode, keep cheap bounds/full-key validation, then skip the repeated
  key-support and raw-exact-filter metadata walks.

## Compatibility Impact

No SQL, error, affected-row, or storage-engine routing behavior should change.
The trusted path is only used after the same direct-update acceptance gates that
already decide whether the handler may bypass the SQL-layer row-read loop.
Unsupported or broader index reads still use the generic defensive path.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Binary-Size And Dependency Impact

One private handler argument and a branch. No new dependency.

## Tests And Verification

- `git diff --check`
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc
  mariadb/storage/mylite/ha_mylite.h`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `archive=build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  - `size_bytes=21182656`
  - `size_mib=20.20`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - 2/2 tests passed.
- `ctest --preset storage-smoke-dev --output-on-failure`
  - 10/10 tests passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000`
  - `prepared primary-key update bind component`: `0.023 us/op`
  - `prepared primary-key update step component`: `2.127 us/op`
  - `prepared primary-key update reset component`: `0.023 us/op`
- Delayed one-second sample during
  `prepared-update-components 10000 1000000`.
  - `mylite_key_uses_raw_exact_filter()` no longer appears under
    `ha_mylite::read_exact_unique_index_row_into()`.
  - It still appears under direct-update proof setup, which remains outside
    this slice.

## Acceptance Criteria

- Accepted direct updates skip repeated generic exact-read eligibility checks.
- Generic exact index reads keep the current validation path.
- Existing direct-update, prepared-update, routed storage, and index tests pass.
- The steady prepared-update sample no longer shows
  `mylite_key_uses_raw_exact_filter()` under the direct-update exact row read.

## Risks

- The trusted flag must stay private to the accepted direct-update path. Passing
  it from generic handler index reads would incorrectly turn defensive
  eligibility checks into assumptions.
