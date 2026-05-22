# Prepared Update Exact-Key Proof Cache

## Problem

Prepared MyLite point updates still call `SQL_SELECT::check_quick()` on every
execute. Even after the accepted direct-update path avoids materializing a
`QUICK_EXACT_KEY_SELECT`, `try_simple_update_unique_key_quick_select()` walks
the same table keys and condition tree to rediscover the same one-part exact
unique-key predicate.

A delayed local sample of
`build/storage-smoke-dev/tools/mylite_perf_baseline
--phase=prepared-update-components 10000 1000000` shows steady-loop samples in:

- `SQL_SELECT::check_quick()`
- `find_simple_unique_key_equal_item()`
- `ha_mylite::info_push(INFO_KIND_MYLITE_UPDATE_EXACT_KEY)`
- `ha_mylite::direct_update_rows_init()`

This slice only targets the SQL-layer exact-key proof rediscovery. Handler-side
direct-update initialization remains a separate follow-up.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `SQL_SELECT::check_quick()` calls
  `try_simple_update_unique_key_quick_select()` before the broader range
  optimizer.
- `try_simple_update_unique_key_quick_select()` clears the current MyLite
  marker, intersects `keys_to_use`, then loops keys and calls
  `find_simple_unique_key_equal_item()` to find the exact predicate.
- `mark_simple_update_unique_key_quick_select()` still evaluates
  `value_item->maybe_null()` / `value_item->is_null()` on every execute, so a
  cached proof can reuse the `Item *` while keeping per-execute NULL parameter
  behavior.
- `Sql_cmd_update::update_single_table()` creates a fresh `SQL_SELECT` through
  `make_select()` for each execution and deletes it before returning. A reusable
  prepared-statement proof therefore has to live on `Sql_cmd_update`, with each
  per-execution `SQL_SELECT` borrowing that cache.

## Design

- Add a small MyLite proof cache to `Sql_cmd_update`:
  - condition pointer,
  - key number,
  - value item pointer,
  - condition-is-key-equality flag.
- Let the per-execution `SQL_SELECT` borrow that cache before `check_quick()`.
- In `try_simple_update_unique_key_quick_select()`, after the usual statement
  shape checks and key-map intersection, try the cache first when MyLite marker
  mode is available.
- Revalidate the cached key number against the current table and `keys_to_use`,
  and keep the existing simple unique-key candidate check before reuse.
- On a cache hit, call `mark_simple_update_unique_key_quick_select()` so NULL
  handling, row estimate, read-time setup, and marker state stay centralized.
- Populate the cache when the normal condition walk finds a marker-compatible
  exact-key predicate.
- Invalidate the cache if the condition pointer, key number, key map, or simple
  unique-key candidate check no longer matches the current execution.

## Compatibility Impact

No SQL result, error, warning, or affected-row behavior should change. The cache
only skips rediscovering the predicate shape; parameter NULL handling and the
direct-update handler acceptance gates still run every execute.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing-policy change. The cache only affects MyLite marker mode for
accepted single-table update planning.

## Binary-Size And Dependency Impact

Adds a few fields and branches to MariaDB-derived update/range planning code.
No dependency.

## Tests And Verification

- `git diff --check`
- `git clang-format --diff -- mariadb/sql/opt_range.cc mariadb/sql/opt_range.h
  mariadb/sql/sql_update.cc mariadb/sql/sql_update.h
  packages/libmylite/tests/embedded_storage_engine_test.c`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `size_bytes=21184568`
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
  - bind: `0.023 us/op`
  - step: `2.147 us/op`
  - reset: `0.023 us/op`
- Delayed steady-loop sample of `prepared-update-components 10000 1000000`
  after the cache was moved to `Sql_cmd_update`:
  - `find_simple_unique_key_equal_item()` no longer appeared.
  - Remaining visible frames included `rewrite_active_update_pages`,
    `find_exact_index_row_id`, `fill_record`, `SQL_SELECT::check_quick`,
    `mylite_direct_update_key_is_supported`, and
    `ha_mylite::direct_update_rows_init()`.

## Acceptance Criteria

- Repeated prepared MyLite exact-key updates can reuse the SQL-layer exact-key
  proof without walking the condition tree.
- Parameter NULL executions still produce zero affected rows through the same
  marker path.
- Generic range optimization and non-MyLite or explain-observable update paths
  keep the existing behavior.
- Existing routed storage and prepared-update tests pass.
- A delayed prepared-update sample no longer shows
  `find_simple_unique_key_equal_item()` as a steady-loop frame under the MyLite
  marker path.

## Risks

- A stale proof cache would be wrong if reused for a different condition. The
  cache is guarded by the current `SQL_SELECT::cond` pointer, key-map
  membership, and the current simple unique-key candidate check. A mismatch
  invalidates the cache.
