# Storage Update Payload Cache Seeding

## Problem

`storage-row-update-components` shows nested checkpoint begin and commit are
cheap, while the direct storage mutation call dominates. Inspection points at a
cache-shape difference: routed SQL usually materializes a row before
`ha_mylite::update_row()` and can seed an active row-payload cache, but direct
storage updates can enter `mylite_storage_update_row_preserving_index_entries()`
without any active row-payload cache. Successful updates then replace a cache
entry only when that cache already exists, so repeated direct updates can keep
falling back to row-page validation.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` calls the
  storage update API after MariaDB has positioned the handler on the current
  row.
- `packages/mylite-storage/src/storage.c::validate_direct_live_row_in_statement_cache()`
  can validate a row from an active row-payload cache without rereading the row
  page or scanning row-state pages.
- `packages/mylite-storage/src/storage.c::replace_active_row_payload_in_cache()`
  updates an existing active cache entry after a successful mutation, but it is
  a no-op when no cache exists.
- `packages/mylite-storage/src/storage.c::store_active_row_payload_in_statement()`
  already creates and seeds an active row-payload cache for resolved active
  statements in read paths.

## Design

- After a successful durable row update, keep the existing fast replacement path
  when an active row-payload cache is already resolved.
- If no active row-payload cache exists but an active cache-owning statement
  exists, seed that statement's row-payload cache with the newly published row
  id and payload.
- Keep this as an opportunistic cache update. Allocation failure must not fail a
  successful storage mutation.
- Do not change row ids, file format, rollback semantics, or durable cache
  promotion.

## Compatibility Impact

No SQL, C API, or storage-engine routing behavior changes. This only changes
process-local storage cache population after successful durable updates.

## Single-File And Embedded Lifecycle Impact

No file lifecycle change. Durable state remains in the `.mylite` file and
existing transaction buffers.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. The improvement applies under the existing routed MyLite
storage engine and direct storage helpers.

## Binary-Size And Dependency Impact

No dependency impact. The library gains only a small cache-seeding branch.

## Tests And Verification

- Run the focused storage tests that cover update, rollback, and row-payload
  caches.
- Build `mylite_perf_baseline`.
- Run `storage-row-update-components` before and after the change.
- Run `prepared-update-components` to confirm routed SQL remains healthy.
- Run `git diff --check` and clang-format diff checks.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-row-update-components 1000 100000`
  reported the mutation component at `7.555 us/op`, down from the previous
  `189.459 us/op` sample, followed by a successful ordered scan.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-row-updates 1000 100000`
  reported aggregate storage row updates at `7.546 us/op`, followed by a
  successful ordered scan.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 100000`
  reported prepared update bind at `0.022 us/op`, step at `2.290 us/op`, and
  reset at `0.036 us/op`, followed by a successful ordered scan.
- `ctest --preset storage-smoke-dev -R 'mylite-storage\.capabilities|libmylite\.embedded-(storage-engine|handler-savepoint|statement)' --output-on-failure`
  passed all 4 selected tests.

## Acceptance Criteria

- Successful direct storage updates seed active row-payload cache state when no
  cache entry existed before the update.
- Existing rollback and row visibility tests pass.
- The direct storage update component benchmark improves materially or the spec
  records why this was not the dominant mutation cost.

## Risks And Unresolved Questions

- Cache updates happen before nested checkpoint commit, matching the existing
  replacement path for already-present caches. A commit failure after a
  successful mutation can still leave process-local cache state ahead of the
  rolled-back nested statement, which is an existing risk for the replacement
  path and should be addressed separately if observed.
- If this is not sufficient, the next split should instrument validation,
  journaling, append/rewrite, and cache-retarget work inside
  `update_row_with_index_entries()`.
