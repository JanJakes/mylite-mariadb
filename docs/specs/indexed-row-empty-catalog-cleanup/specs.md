# Indexed Row Empty Catalog Cleanup

## Problem

Prepared row-only updates read the target row through MyLite's exact indexed-row
path before mutating it. On the hot cache-hit path, table metadata and exact
index state usually come from active statement caches, so
`find_indexed_row_payload_with_header()` and `find_exact_index_row_id()` do not
materialize a local catalog image. They still call `free_catalog_image()` on an
empty stack image before returning.

A fresh local sample of
`prepared-row-only-update-components --profile-iterations=10000000 10000`
showed `free_catalog_image()` below `find_indexed_row_payload_with_header()` on
the accepted direct-update target-row read path. The call only reaches
`free(NULL)`, but it sits in the per-row prepared update loop.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()` reads
  the exact unique target row before applying the accepted direct update.
- `packages/mylite-storage/src/storage.c::find_indexed_row_payload_with_header()`
  can satisfy table-entry, exact-index, and row-payload work from active
  statement caches without allocating its local `mylite_storage_catalog_image`.
- `packages/mylite-storage/src/storage.c::find_exact_index_row_id()` follows the
  same shape when an active exact-index cache hit avoids catalog fallback work.
- `free_catalog_image()` is still correct for empty images, but avoiding the call
  entirely removes a useless function call and `free(NULL)` from the hot path.

## Design

- Guard local catalog cleanup in the indexed-row payload and exact-index row-id
  helpers with `catalog.bytes != NULL`.
- Leave all allocated catalog cleanup unchanged.
- Do not change catalog cache ownership, statement cache lifetime, row
  visibility, exact-index lookup semantics, or storage API behavior.

## Compatibility Impact

No SQL-visible behavior change is intended. The slice only skips cleanup for
stack-local catalog images that were never allocated.

## Single-File And Embedded Lifecycle Impact

No file-format, journal, lock, sidecar, or open/close lifecycle change.

## Public API And Storage Routing Impact

No public `libmylite` API or storage-engine routing change.

## Binary-Size And Dependency Impact

No dependency change. Binary-size impact is limited to a few branch checks in
storage hot paths.

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
  10000` twice:
  - first run: bind `0.022 us/op`, step `1.534 us/op`, reset `0.021 us/op`,
    checksum `5000000`
  - second run: bind `0.021 us/op`, step `1.542 us/op`, reset `0.021 us/op`,
    checksum `5000000`

## Acceptance Criteria

- Hot indexed-row cache-hit paths no longer call `free_catalog_image()` for
  never-allocated local catalog images.
- Allocated catalog images are still released exactly once.
- Existing storage and routed embedded tests pass.
- The prepared row-only update component benchmark is stable or faster.

## Risks And Unresolved Questions

- This is a small cleanup skip, not a substitute for the larger prepared-DML
  execution reuse, table-open, or `JOIN::prepare()` work still visible in the
  profile.
