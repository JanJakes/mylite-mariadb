# Active Live Row ID Cache Absence

## Problem

Active row updates call `seed_active_live_row_id_cache_in_statement()` before
validating the target row. When no durable live-row-id cache exists for the
table, the current active statement repeats the same durable cache lookup on
every update in the statement.

A local prepared-update sample shows this repeated probe under:

```text
update_row_with_index_entries
  load_active_live_row_id_cache_in_statement
    find_durable_live_row_id_cache
```

The lookup is small compared with the full update path, but it is pure control
work once the statement has already established that no matching durable
live-row-id cache exists for the table and header generation.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` routes the hot
  prepared update through `mylite_storage_update_row_with_index_entry_changes()`.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries()`
  calls `seed_active_live_row_id_cache_in_statement()` before row validation.
- `packages/mylite-storage/src/storage.c::seed_active_live_row_id_cache_in_statement()`
  already skips work when an active live-row-id cache exists.
- `packages/mylite-storage/src/storage.c::load_active_live_row_id_cache_in_statement()`
  probes `find_durable_live_row_id_cache()` and returns when no durable cache
  exists, but it does not remember that negative result.
- The statement already has a precedent for one-table negative catalog facts in
  `mylite_storage_table_index_root_absence_cache`.

## Scope

- Add a small statement-local absence cache for durable live-row-id cache
  lookups.
- Key the absence by catalog root page, catalog generation, and table id. The
  active statement already belongs to one primary file.
- Clear the absence with normal statement cleanup, catalog-root cache clearing,
  and reusable-statement reset.
- Leave durable cache creation, promotion, and invalidation semantics unchanged.

## Non-Goals

- No change to durable live-row-id cache capacity or promotion.
- No change to row visibility, checkpoint rollback, or cache correctness.
- No public API or file-format change.
- No attempt to solve the larger secondary-key update rewrite costs in this
  slice.

## Design

Add a `mylite_storage_live_row_id_cache_absence_cache` to
`mylite_storage_statement`, with the same shape as the existing table-index-root
absence cache.

`seed_active_live_row_id_cache_in_statement()` should:

1. Return when an active cache already exists.
2. Return when the statement has a matching durable-cache absence.
3. Otherwise call `load_active_live_row_id_cache_in_statement()`.

`load_active_live_row_id_cache_in_statement()` should record a matching absence
when `find_durable_live_row_id_cache()` returns `NULL`.

The absence is only a statement-local negative cache. Later durable cache
creation in another statement is naturally visible because the current
statement ends before the next statement can use its cache set.

## Compatibility Impact

No SQL, C API, or storage-engine compatibility behavior changes. The absence
cache only avoids repeating a lookup that has already returned no durable cache
inside the same statement view.

## Single-File And Embedded-Lifecycle Impact

No durable file or lifecycle change. The new state is transient statement heap
state and is discarded with the active statement.

## Build, Size, And Dependencies

No dependency or binary-profile change. The change adds one small struct field
to active storage statements.

## Test Plan

- Existing storage update, rollback, and cache tests cover row visibility and
  cache invalidation behavior.
- `git diff --check`
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_perf_baseline`
- `ctest --preset storage-smoke-dev -R 'mylite-storage\.capabilities' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 1000000`

## Acceptance Criteria

- Active statements remember that no durable live-row-id cache exists for a
  table under the current header identity.
- Existing storage capability coverage passes.
- Prepared-update component samples no longer show repeated durable
  live-row-id cache absence probes as a visible update-path sample.

## Risks

- A stale absence would skip seeding from a durable cache. Keeping the cache
  statement-local and keyed by the same header identity limits that risk.
