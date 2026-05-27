# Direct Update Cache-Hit Reset Elision

## Problem

The VPS verification run after the maintained-update no-plan cache showed that
MyLite storage mutation is no longer the dominant prepared-update cost in this
environment: `storage-indexed-row-update-components` measured about 2.229 us/op
for the storage mutation component, while `prepared-update-components` measured
about 207.693 us/op for the SQL prepared update step.

In the prepared direct-update handler path, `ha_mylite::direct_update_rows_init()`
already checks the accepted direct-update shape cache before repeating
key-safety and FK gates. However it still clears compact snapshot state before
that cache hit. Stable prepared row-only updates that hit the shape cache do
not need that reset, because `use_direct_update_shape_cache()` immediately
installs the cached snapshot shape.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc::Sql_cmd_update::execute_mylite_prepared_direct_update()`
  pushes the exact-key proof, update fields, and update values before calling
  `handler::direct_update_rows_init()`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::info_push()` resets
  direct-update shape state when the exact key, update-field list, or
  update-value list is pushed.
- `ha_mylite::direct_update_rows_init()` performs conservative table and
  metadata gates, clears compact snapshot state, and then calls
  `use_direct_update_shape_cache()`.
- `ha_mylite::use_direct_update_shape_cache()` copies the cached row-compare,
  duplicate-key, index-change, compact snapshot, key-change, and snapshot-field
  facts into the current handler state on a hit.

## Design

Move the compact snapshot state reset in `direct_update_rows_init()` from the
pre-cache-hit path to the cache-miss path.

On a shape-cache hit, return immediately after `use_direct_update_shape_cache()`
installs the cached facts. On a miss, keep the existing reset and recomputation
path unchanged before the updated-field key-safety scan, FK gates, direct-update
shape computation, compact snapshot preparation, and cache store.

This does not skip `info_push()` reset work, exact-key proof validation, table
gates, metadata checks, FK gates for key-changing updates, or storage mutation.

## Affected Subsystems

- MyLite MariaDB storage handler direct-update initialization.
- Prepared exact-key row-DML performance for stable handler shape-cache hits.

## Compatibility Impact

No SQL-visible behavior change is intended. Shape-cache misses run the same
reset and recomputation path as before. Unsupported direct-update shapes still
return `HA_ERR_WRONG_COMMAND` and fall back to the normal MariaDB path.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file-format, sidecar, journal, lock, recovery, or embedded
lifecycle change. The change only reorders transient handler state cleanup
inside one direct-update initialization call.

## Public API And File-Format Impact

No public C API, SQL surface, or file-format change.

## Binary-Size And Dependency Impact

No new dependency and negligible binary-size impact.

## Tests And Verification Plan

- `git diff --check`
- `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-row-only-update-components 1000 100000`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 100000`

## Verification Results

Verified on the VPS handoff environment on 2026-05-27:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc`:
  passed.
- `cmake --build build/mariadb-mylite-storage-smoke --target libmariadbd.a`:
  passed and rebuilt `ha_mylite.cc`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`:
  passed in 14.17 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-row-only-update-components 1000 100000`:
  passed; the row-only update step measured 215.600 us/op, maintained-root
  plans remained `0`, no-plan cache hits and stores remained `0`, active
  row-only rewrite successes were `99000`, and append update writes were `0`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 100000`:
  passed; the key-changing update step measured 255.561 us/op, maintained-root
  plans were `1000`, no-plan cache hits were `99000`, stores were `1000`,
  active rewrite successes were `99000`, and append update writes were `0`.

## Acceptance Criteria

- Stable direct-update shape-cache hits return without clearing compact
  snapshot state first.
- Shape-cache misses still clear compact snapshot state before recomputing the
  direct-update shape.
- Existing embedded storage-engine prepared update coverage passes.
- Prepared update and row-only update benchmark phases continue to complete
  with no behavior regressions.

## Risks And Unresolved Questions

- The change assumes `use_direct_update_shape_cache()` fully installs compact
  snapshot state on hit. That is true today because it copies
  `direct_update_shape_cache_can_use_compact_snapshot`,
  `direct_update_shape_cache_snapshot_field_count`,
  `direct_update_shape_cache_snapshot_byte_count`, and
  `direct_update_shape_cache_snapshot_fields`.
- This does not address the broader SQL-layer prepared-update cost. Table
  opening, locking, and any future DML cleanup reuse remain separate higher-risk
  slices.
