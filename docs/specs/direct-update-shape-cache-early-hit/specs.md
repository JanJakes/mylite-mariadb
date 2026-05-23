# Direct Update Shape Cache Early Hit

## Problem

Prepared row-only `UPDATE` loops now spend more time in MariaDB SQL and handler
setup than in the MyLite storage row rewrite. A fresh 2026-05-23 sample of
`prepared-row-only-update-components` still shows
`ha_mylite::direct_update_rows_init()` in the steady loop, mainly around
immutable direct-update admission work.

The existing handler direct-update shape cache stores accepted non-key-changing
write sets for the same `TABLE_SHARE`, write bitmap, and key count, but
`direct_update_rows_init()` checks that cache only after repeating the
updated-field key-safety scan and any key-change FK gates.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` pushes the
  MyLite exact-key proof, update fields, and update values before calling
  `handler::direct_update_rows_init()`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows_init()`
  first validates ordinary handler/table gates, then calls
  `mylite_update_fields_change_direct_unsafe_key()`, optionally checks FK
  presence for key-changing updates, and only then consults
  `use_direct_update_shape_cache()`.
- `ha_mylite::store_direct_update_shape_cache()` stores the cache only when the
  accepted update fields do not change direct-unsafe key parts.
- `ha_mylite::use_direct_update_shape_cache()` requires the same table share,
  same key count, and same write-set bitmap before copying cached row-compare,
  duplicate-check, index-change, and per-index key-change facts back into the
  current direct-update state.

## Design

Move the direct-update shape-cache check earlier in
`direct_update_rows_init()`, after conservative table, file, view, and in-server
constraint gates but before the repeated updated-field key-safety scan.

On a cache hit, return success immediately with the cached non-key-changing
shape facts installed. On a miss, keep the existing path unchanged: recompute
whether update fields touch unsafe keys, run FK gates for key-changing updates,
compute the direct-update shape facts, and store the cache only for
non-key-changing updates.

## Affected Subsystems

- MyLite MariaDB storage handler direct-update initialization.
- Prepared exact-key row-DML performance for stable non-key updates.

No SQL parser, optimizer, storage format, public API, or catalog behavior is
changed.

## Compatibility Impact

No SQL-visible behavior change is intended. Unsupported shapes still fall back
through the existing direct-update admission path. Key-changing updates remain
uncached, so FK presence and unique-key-sensitive gates are still evaluated on
every execution for those shapes.

## Single-File And Embedded Lifecycle Impact

No `.mylite` file-format, sidecar, journal, lock, or recovery change. The cache
remains process-local handler state and is cleared with the handler lifecycle.

## Public API And File-Format Impact

No public `libmylite`, storage C API, or `.mylite` file-format change.

## Test Plan

- Passed `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc`.
- Passed `git diff --check`.
- Passed `cmake --build build/mariadb-mylite-storage-smoke --target
  libmariadbd.a`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_perf_baseline mylite_embedded_storage_engine_test
  mylite_embedded_statement_test`.
- Passed `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000` during a sampled
  run:
  - bind: `0.021 us/op`
  - step: `1.650 us/op`
  - reset: `0.021 us/op`
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-miss-components 10000 1000000`:
  - bind: `0.022 us/op`
  - step: `1.118 us/op`
  - reset: `0.021 us/op`
  - row-only miss checksum: `0`
- A two-second `sample` of the clean prepared row-only update run no longer
  shows `ha_mylite::direct_update_rows_init()` under the hot
  `Sql_cmd_update::update_single_table()` path. Timing remains within local
  noise; the main value of this slice is removing repeated handler admission
  work before larger prepared-DML reuse work.

## Acceptance Criteria

- Repeated non-key-changing prepared exact-key updates hit the direct-update
  shape cache before updated-field key-safety and FK-gate work.
- Key-changing direct updates still miss the cache and run the existing gates.
- Existing routed storage tests and prepared-statement tests pass.
- Prepared row-only update timing does not regress.
