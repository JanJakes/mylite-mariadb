# Direct Update Key Support Cache

## Problem

Accepted prepared MyLite point updates still call
`mylite_direct_update_key_is_supported()` from
`ha_mylite::info_push(INFO_KIND_MYLITE_UPDATE_EXACT_KEY)` on every execution.
A delayed local sample of `prepared-update-components 10000 1000000` after the
SQL-layer proof cache showed this function below `info_push()`.

This slice targets only the handler metadata proof. Row lookup, row fill, and
storage mutation remain separate follow-ups.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `ha_mylite::info_push()` receives an already accepted exact-key proof from
  the SQL layer, but still verifies the target key with
  `mylite_direct_update_key_is_supported()`.
- The key-support predicate depends on stable handler-open table/key metadata:
  key shape, uniqueness/nullability flags, raw exact-filter support, and key
  field layout.
- `ha_mylite::open()` already caches table-shape properties such as BLOB
  presence, auto-increment field, and row-lifecycle support.

## Design

- Add a per-handler `direct_update_key_supported[MAX_KEY]` cache.
- Populate it in `ha_mylite::open()` for handlers with `table->s->keys <=
  MAX_KEY`, using the existing `mylite_direct_update_key_is_supported()`
  predicate.
- Clear it in the constructor and `close()`.
- In `info_push(INFO_KIND_MYLITE_UPDATE_EXACT_KEY)`, validate the accepted key
  number against the cached support bitmap instead of recomputing the full key
  proof on every execution.

## Compatibility Impact

No SQL result, warning, error, or affected-row behavior should change. The cache
only memoizes a handler-open metadata proof and preserves the existing
`HA_ERR_WRONG_COMMAND` fallback when a key is unsupported.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing-policy change.

## Tests And Verification

- `git diff --check`
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc
  mariadb/storage/mylite/ha_mylite.h`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  all -DPLUGIN_MYLITE_SE=STATIC`
  - `size_bytes=21184592`
  - `size_mib=20.20`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `2/2` tests passed.
- `ctest --preset storage-smoke-dev --output-on-failure`
  - `10/10` tests passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000`
  - prepared primary-key update bind component: `0.023 us/op`
  - prepared primary-key update step component: `2.108 us/op`
  - prepared primary-key update reset component: `0.023 us/op`
- Delayed local sample of the same phase no longer showed
  `mylite_direct_update_key_is_supported()` or
  `mylite_key_uses_raw_exact_filter()` below hot prepared execution. Remaining
  sampled frames included `rewrite_active_update_pages`,
  `find_indexed_row_payload`, `find_exact_index_row_id`,
  `open_existing_file_scope`, `close_existing_file_scope`, `fill_record`,
  `ha_mylite::direct_update_rows_init()`, `SQL_SELECT::check_quick()`, and
  `ha_mylite::info_push()`.

## Acceptance Criteria

- Accepted direct-update `info_push()` no longer recomputes the full
  direct-update key-support proof on hot prepared point-update loops.
- Unsupported key shapes still reject direct update through the existing
  fallback path.
- Existing routed storage and prepared-update tests pass.

## Risks

- A stale key-support cache would be wrong if table key metadata changed under
  an open handler. MariaDB reopens handlers for table-definition changes; the
  cache is scoped to a single `ha_mylite::open()` lifetime and cleared on
  `close()`.
