# Preserved Index Update Plan Skip

## Problem

Prepared row-only updates that keep every index entry unchanged call
`update_row_with_index_entries_for_context()` with `preserve_index_entries`
set. Those calls pass no index entries and cannot produce a maintained
index-root update plan. The hot path still initialized an empty
`mylite_storage_maintained_index_update_plan`, checked it for emptiness before
active and inline row rewrites, passed its zero protected-page count to journal
startup, called the maintained-root writers with zero work, and cleared the
empty plan at exit.

The behavior is correct, but the latest local
`prepared-row-only-update-components` sample still shows
`update_row_with_index_entries_for_context()` as the dominant storage mutation
frame after the previous handler-side direct-update cleanup.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()`
  passes `preserve_index_entries=true` for accepted row-only direct updates
  that keep key images stable.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries_for_context()`
  rejects any `preserve_index_entries` call that also supplies index entries,
  an index-entry count, or an index-entry change bitmap.
- Maintained index-root planning is only needed when `index_entry_count != 0`
  and the active statement has not already recorded table index roots as
  absent.
- `plan_maintained_index_root_updates()` initializes and owns the update-plan
  scratch buffers before it can allocate any non-inline arrays.

## Design

- Track whether `update_row_with_index_entries_for_context()` actually invoked
  maintained index-root planning.
- Leave the stack-local plan uninitialized on preserved-index row-only updates
  and other no-index-entry updates.
- Treat a missing plan as empty for active buffered rewrites, inline row
  rewrites, and journal protected-page setup.
- Use the maintained plan's changed-entry bitmap only when a plan exists.
- Skip maintained-root writer calls and plan cleanup when no plan exists.
- Leave planned index-update behavior unchanged for indexed updates that do
  need maintained root planning.

## Affected Subsystems

- First-party MyLite storage row update execution.
- Prepared row-only update performance evidence.

## Compatibility Impact

No SQL-visible behavior change is intended. Preserved-index row-only updates
already had no maintained root work to perform, and indexed updates that can
publish maintained root changes still plan, journal, write, and clean up the
same maintained-root state.

## Single-File And Embedded Lifecycle Impact

No file-format, journal format, lock, recovery, sidecar, or embedded lifecycle
change. The journal still protects the same pages for planned maintained-root
updates and receives no protected-page list for row-only no-plan updates.

## Public API And File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

## Binary-Size And Dependency Impact

No new dependency. Binary-size impact is limited to storage-layer branching in
one row-update path.

## Test And Verification

- Passed `git diff --check`.
- Passed `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`.
- Passed `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Passed focused storage and routed embedded storage-engine CTest coverage with
  `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Passed full `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`:
  - bind: `0.022 us/op`
  - step: `1.552 us/op`
  - reset: `0.021 us/op`

## Acceptance Criteria

- Preserved-index row-only updates do not initialize, inspect, write, or clear a
  maintained index-root update plan.
- Indexed updates that need maintained root planning keep existing behavior.
- Existing storage-smoke tests pass.
- Local prepared row-only update timing is stable or faster.

## Risks And Unresolved Questions

- The expected gain is small because the remaining dominant storage cost is row
  payload mutation and exact-index row lookup. Larger prepared-DML wins still
  require the broader prepared direct-update rebind work.
