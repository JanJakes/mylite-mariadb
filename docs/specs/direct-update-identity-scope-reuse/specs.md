# Direct Update Identity Scope Reuse

## Problem

A current `prepared-row-only-update-components` sample still shows the accepted
MyLite direct-update path spending visible time in storage filename and table
name identity-scope setup. The direct update flow performs one exact unique-key
read and then one storage update for the same primary file and table:

1. `ha_mylite::direct_update_rows()` calls
   `read_exact_unique_index_row_into()`.
2. After SQL condition and assignment evaluation, it calls `update_row()`.
3. Both helpers independently establish identical
   `Mylite_filename_identity_scope` and `Mylite_table_name_identity_scope`
   objects.

These scopes only set and restore thread-local identity pointers used by hot
storage caches to avoid repeated string comparisons. For an accepted direct
update, the filename, schema, and table pointers are stable across the exact
read, SQL row evaluation, and update.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()`
  builds the exact key, reads the matching row through
  `read_exact_unique_index_row_into()`, evaluates the condition and update
  values, and then calls `update_row()`.
- `ha_mylite::read_exact_unique_index_row_into()` establishes filename and
  table-name identity scopes before calling
  `mylite_storage_find_indexed_row_in_statement_into()` or the non-statement
  equivalent.
- `ha_mylite::update_row()` establishes the same identity scopes before
  duplicate-key, FK, autoincrement, row-payload, and storage update calls.
- `packages/mylite-storage/src/storage.c` identity scopes store previous
  thread-local values and restore them on scope end. Nested identical scopes
  are correct but redundant on the direct-update path.
- `direct_update_row_in_progress` already marks the handler state used by
  `update_row()` for direct-update-specific key-change and duplicate-check
  decisions.

## Design

Hoist the identity scopes to `ha_mylite::direct_update_rows()` after the runtime
key is built and the primary file, schema, and table names have been resolved.
Keep the scopes active across the exact read and final update.

Extend the small C++ RAII wrappers so callers can disable them explicitly.
`read_exact_unique_index_row_into()` and `update_row()` continue to create their
own scopes for normal callers, but skip those inner scopes while
`direct_update_row_in_progress` is true. The direct-update outer scope then owns
the thread-local identity lifetime.

Set `direct_update_row_in_progress` before the exact read and restore the
previous value through a local guard on every exit. This widens the flag
lifetime inside `direct_update_rows()` but does not change storage semantics:
the flag only affects MyLite handler hot-path decisions, and all
direct-update-only decisions remain inside the same accepted direct-update
operation.

## Compatibility Impact

No SQL-visible behavior change is intended. The same handler exact read,
condition evaluation, assignment evaluation, CHECK/FK/autoincrement handling,
and storage update path still run. Unsupported direct-update shapes continue to
fall back before this path.

## Single-File And Embedded Lifecycle Impact

No file-format or sidecar lifecycle change. The scopes remain thread-local and
live only for the duration of one direct-update handler call.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size And Dependency Impact

No new dependency. The code change is limited to the static MyLite handler.

## Test And Verification Plan

- Passed `git clang-format --diff --
  mariadb/storage/mylite/ha_mylite.cc`.
- Passed `git diff --check`.
- Passed `cmake --build build/mariadb-mylite-storage-smoke --target
  libmariadbd.a -j1`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_embedded_statement_test
  mylite_perf_baseline -j1`.
- Passed `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-statement|libmylite.embedded-storage-engine'
  --output-on-failure`.
- Passed `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`:
  - bind: `0.022 us/op`
  - step: `1.632 us/op`
  - reset: `0.022 us/op`
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-miss-components 10000 1000000`:
  - bind: `0.022 us/op`
  - step: `1.129 us/op`
  - reset: `0.022 us/op`
  - row-only miss checksum: `0`
- Ran a sampled `prepared-row-only-update-components` run with
  `--profile-iterations=5000000`:
  - bind: `0.022 us/op`
  - step: `1.612 us/op`
  - reset: `0.023 us/op`
  - profile shows the remaining identity-scope samples under
    `ha_mylite::direct_update_rows()`, not as duplicate nested
    `read_exact_unique_index_row_into()` / `update_row()` scope setup.

## Acceptance Criteria

- Accepted direct updates reuse one filename/table identity scope across the
  exact read and update mutation.
- Non-direct reads and updates keep their existing local identity scopes.
- Existing storage-smoke tests pass.
- Prepared row-only update timing does not regress.

## Risks

- `direct_update_row_in_progress` becomes true earlier inside
  `direct_update_rows()`. The implementation must keep this limited to the
  accepted direct-update call and restore the previous flag value on every
  exit.
- Inner helpers must only skip their local scopes when an outer direct-update
  scope is active.
