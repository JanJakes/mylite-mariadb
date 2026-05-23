# Row Update Empty Catalog Cleanup

## Problem

Prepared row-only updates enter `update_row_with_index_entries_for_context()`
after the direct-update hook has read and validated the target row. On the hot
row-only path, maintained index-root planning is skipped and the local catalog
image remains empty, but cleanup still calls `free_catalog_image()` before the
storage update returns.

The call is correct for an empty image, but it reaches `free(NULL)` inside the
per-row prepared update loop. A focused post-cleanup local sample of
`prepared-row-only-update-components --profile-iterations=10000000 10000`
still showed `free_catalog_image()` under
`update_row_with_index_entries_for_context()`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()` calls
  the MyLite row-update API after MariaDB has accepted a direct update.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries_for_context()`
  initializes a stack-local `mylite_storage_catalog_image catalog = {0}`.
- That function reads the catalog only when maintained index-root planning is
  needed. Row-only stable-key updates with no maintained root plan leave
  `catalog.bytes == NULL`.
- The same function unconditionally freed the local catalog image at exit.

## Design

- Guard the row-update cleanup with `catalog.bytes != NULL`.
- Leave all allocated catalog cleanup unchanged.
- Keep maintained index-root planning, active row rewrite, row-payload cache
  maintenance, exact-index cache maintenance, and durable cache retargeting
  behavior unchanged.

## Compatibility Impact

No SQL-visible behavior change is intended. The slice only skips cleanup for a
stack-local catalog image that was never allocated.

## Single-File And Embedded Lifecycle Impact

No file-format, journal, lock, sidecar, or open/close lifecycle change.

## Public API And Storage Routing Impact

No public `libmylite` API or storage-engine routing change.

## Binary-Size And Dependency Impact

No dependency change. Binary-size impact is limited to one branch in the storage
row-update cleanup path.

## Tests And Verification

- Passed `git diff --check`.
- Passed `git clang-format --diff -- packages/mylite-storage/src/storage.c`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline
  -j1`.
- Passed `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Passed `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Passed `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components --profile-iterations=5000000
  10000` twice with the guarded cleanup:
  - first run: bind `0.021 us/op`, step `1.547 us/op`, reset `0.021 us/op`,
    checksum `5000000`
  - second run: bind `0.021 us/op`, step `1.555 us/op`, reset `0.021 us/op`,
    checksum `5000000`
- Alternated back to the unguarded cleanup once:
  - unguarded run: bind `0.022 us/op`, step `1.589 us/op`, reset `0.022 us/op`,
    checksum `5000000`

## Acceptance Criteria

- Row-only hot updates do not call `free_catalog_image()` for a never-allocated
  local catalog image.
- Allocated catalog images are still released exactly once.
- Existing storage and routed embedded tests pass.
- The prepared row-only update component benchmark is stable or faster.

## Risks And Unresolved Questions

- The measured gain is small and local. Larger prepared-DML wins still require
  reducing repeated MariaDB table-open, context-analysis, and `JOIN::prepare()`
  work with a separate compatibility-backed design.
