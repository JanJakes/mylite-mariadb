# Transaction Live Row Validation Cache

## Problem

Nested row-update statements validate the target row through the top active
statement. In transaction workloads that open one nested statement per row
mutation, that means each nested statement starts with an empty live-row
validation cache and repeats `row_is_hidden_after()` scans across row-state
pages.

A local storage-update sample shows the hot path under:

```text
update_row_with_index_entries
  load_direct_live_row_in_statement_cache
    row_is_hidden_after
      read_row_state_page
```

The exact-index and row-payload caches already use the outer active cache
statement so committed nested updates can share cache state across the
transaction. Live-row validation should use the same cache scope while keeping
savepoint rollback invalidation intact. The first validation miss should also
materialize the table row-state map once for the active statement instead of
scanning row-state pages separately for every cold row id.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` routes handler
  updates through `mylite_storage_update_row_with_index_entry_changes()`.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries()`
  resolves `active_cache_statement` for exact-index and row-payload caches, but
  resolves live-row validation through `active_file_statement`.
- `packages/mylite-storage/src/storage.c::validate_direct_live_row_in_statement_cache()`
  skips row-state scans when the provided live-row cache has already validated
  the row id.
- `packages/mylite-storage/src/storage.c::build_row_state_map()` already builds
  a hashed table-local source-row map for full scans and exact-index
  visibility.
- `packages/mylite-storage/src/storage.c::mylite_storage_rollback_statement()`
  clears the parent statement chain's live-row, live-row-id, exact-index, and
  row-payload caches on nested rollback.

## Scope

- Use the active cache statement for row-update live-row validation and
  replacement maintenance.
- Cache a table row-state map in the active cache statement on the first direct
  live-row validation miss, and update that map when row update/delete
  mutations add row-state entries.
- Keep fallback behavior unchanged when no active cache statement exists.
- Add focused storage test hooks and a rollback test proving parent live-row
  and row-state map caches are cleared when a nested update rolls back.
- Update roadmap notes with the performance behavior.

## Non-Goals

- No public API or durable file-format change.
- No change to row-state page encoding or recovery journals.
- No new durable live-row cache behavior.
- No attempt to optimize delete validation in this slice.

## Design

`update_row_with_index_entries()` already computes:

- `active_file_statement`, the current active statement for the open file.
- `active_cache_statement`, the outer statement used for exact-index and
  row-payload caches.

Resolve `active_live_row_cache` from `active_cache_statement`, pass
`active_cache_statement` into `validate_direct_live_row_in_statement_cache()`,
and pass the same statement into `replace_active_live_row_in_cache()`.

This lets successful nested row updates retain validated live-row state in the
transaction-level cache. If a nested statement rolls back after mutating that
cache, existing rollback handling clears the parent statement chain caches,
which removes any speculative live-row entries before the parent continues.

When direct validation still needs row-state visibility, build and cache the
table row-state map on the active cache statement. Subsequent cold validations
answer source-row hidden checks from the hash map. Successful updates and
deletes record their new row-state entry into the cached map when it exists;
rollback clears the parent map cache together with the other speculative
visibility caches.

## Compatibility Impact

No SQL, C API, or storage-engine compatibility behavior changes. The same row
visibility checks remain authoritative on a cache miss, and nested rollback
still restores storage bytes and clears parent caches.

## Single-File And Embedded-Lifecycle Impact

No file lifecycle change. The cache is transient statement heap state and is
discarded or promoted through the existing statement lifecycle.

## Build, Size, And Dependencies

No dependency or binary-profile change. The test-only hook is compiled only
under `MYLITE_STORAGE_TEST_HOOKS`.

## Test Plan

- Add a storage test covering nested update rollback after the parent live-row
  and row-state map caches have been populated.
- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_perf_baseline`
- `ctest --preset storage-smoke-dev -R 'mylite-storage\.capabilities' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-row-update-components 1000 1000000`

## Acceptance Criteria

- Repeated nested row updates in one transaction can reuse live-row validation
  cache state after committed nested statements.
- Cold validation misses build one statement-local row-state map per table view
  instead of scanning row-state pages per row id.
- Rolling back a nested update clears the parent live-row and row-state map
  caches before the parent transaction continues.
- Existing storage rollback and row visibility tests continue to pass.
- Storage row-update component samples no longer show repeated row-state scans
  as the dominant mutation-path cost after the first map build.

## Risks

- Using the outer cache scope before a nested statement commits can make the
  cache speculative. Existing nested rollback cache clearing is therefore part
  of the correctness contract and is covered by the new test.
- If row-state map maintenance misses a mutation path, direct validation could
  read stale hidden-row state. Update and delete paths maintain the map in this
  slice; existing visibility tests cover the broader mutation behavior.
