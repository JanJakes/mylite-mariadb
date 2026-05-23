# Row-Only Active Update Cache Bookkeeping

## Problem

The prepared row-only update benchmark reaches MyLite's active in-place rewrite
path after the handler has already materialized the target row. That rewrite
keeps the same row id and does not advance the active header, but the generic
storage update path still performs bookkeeping for append-style replacements:

- seed the active live-row-id cache before it knows whether the update will
  append a replacement row,
- call exact-index cache row-id retargeting even when the row id is unchanged,
- refresh the same deferred durable-cache retarget marker on every in-place
  rewrite in one active statement.

Those operations are small individually, but they sit on the hottest row-only
prepared update path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()`
  routes accepted exact-key updates through
  `ha_mylite::update_row()` after `fill_record()` evaluates the updated row.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries_for_context()`
  handles preserving-index updates for both append fallback and active
  in-place rewrites.
- `rewrite_active_update_pages()` returns `used_active_update_rewrite` when the
  current buffered row/state shape can be rewritten in place. For row-only
  updates it sets `position.row_page_id` to the original `row_id`.
- Active live-row-id cache seeding is only required before append-style
  replacement changes a row id. It is unnecessary when the active rewrite keeps
  the row id unchanged.
- Durable cache retargeting is deferred on active statements and later applied
  at statement/transaction commit boundaries. Repeating the same table/header
  marker in the same statement does not change the later cache action.

## Design

- Delay active live-row-id cache seeding until after the active rewrite attempt.
- Skip that seeding when `rewrite_active_update_pages()` handles the mutation in
  place.
- Avoid exact-index row-id retargeting for preserve-index updates when the old
  and new row ids are identical.
- Add a small deferred-retarget matcher so repeated in-place rewrites for the
  same table and header fingerprint do not rewrite the same deferred durable
  cache marker.

The append fallback keeps the existing seed-before-append behavior so any
loaded live-row-id cache can still be maintained when an update publishes a
replacement row id.

## Compatibility Impact

No SQL-visible behavior change is intended. Row ids, row payloads, index
entries, transaction visibility, rollback, and durable cache invalidation
semantics remain unchanged. The slice only skips bookkeeping that has no effect
for same-row-id active rewrites.

## Single-File And Embedded Lifecycle Impact

No file-format, journal, lock, sidecar, or open/close lifecycle change.

## Public API And Storage Routing Impact

No public `libmylite` API or storage-engine routing change.

## Binary-Size And Dependency Impact

Adds one small internal helper and no dependency.

## Tests And Verification

- Passed `git diff --check`.
- Passed `git clang-format --diff -- packages/mylite-storage/src/storage.c`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_storage_test mylite_embedded_storage_engine_test
  mylite_perf_baseline -j1`.
- Passed `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Passed `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Passed `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`:
  - bind: `0.023 us/op`
  - step: `1.724 us/op`
  - reset: `0.023 us/op`
  - checksum: `1000000`
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components --profile-iterations=5000000
  10000`:
  - bind: `0.022 us/op`
  - step: `1.621 us/op`
  - reset: `0.022 us/op`
  - checksum: `5000000`

## Acceptance Criteria

- Active in-place row-only rewrites no longer seed live-row-id caches that they
  cannot change.
- Preserve-index in-place rewrites do not call exact-index row-id retargeting
  for identical row ids.
- Repeated active in-place rewrites keep one deferred durable-cache retarget
  marker for the same table/header fingerprint.
- Existing storage and routed embedded tests pass.

## Risks And Unresolved Questions

- The deferred durable-cache marker comparison intentionally uses the cache
  retarget fingerprint fields: catalog root page, catalog generation, and page
  count. If future durable caches depend on additional header fields, that
  helper must be extended with the new fingerprint.
