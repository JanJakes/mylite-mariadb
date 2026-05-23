# Combined Storage Identity Scope

## Problem

The direct-update hot path sets two storage identity scopes before calling into
the storage layer: one for the primary filename pointer and one for the
schema/table-name pointers. The scopes let storage cache lookups use pointer
identity instead of repeated string comparison, but paired call sites currently
enter and exit them through separate C API calls.

A fresh local `prepared-row-only-update-components` sample after preserved-index
update-plan cleanup still shows `mylite_storage_begin_filename_identity_scope`,
`mylite_storage_end_filename_identity_scope`,
`mylite_storage_begin_table_name_identity_scope`, and
`mylite_storage_end_table_name_identity_scope` on the prepared direct-update
path. The cost is small, but it is pure control-plane work around every row.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()`
  always needs both filename and table-name identity scopes around the exact
  row lookup and row-only storage update.
- `ha_mylite::read_exact_unique_index_row_into()` and `ha_mylite::update_row()`
  also need both scopes when they are not already running under the outer
  direct-update row scope.
- `packages/mylite-storage/src/storage.c` keeps filename and table-name
  identity state in separate thread-local slots. The paired scope can save and
  restore both slots directly without changing cache semantics.

## Design

- Add `mylite_storage_identity_scope` to the internal storage header.
- Add `mylite_storage_begin_identity_scope()` and
  `mylite_storage_end_identity_scope()` to save, set, and restore filename plus
  table-name identity slots together.
- Add a small RAII wrapper in the MyLite handler for paired scopes.
- Replace paired filename/table-name scope use in direct update, exact unique
  row reads, and row updates with the combined scope.
- Leave the existing filename-only and table-name-only scopes available for
  callers that only need one side.

## Affected Subsystems

- MyLite storage internal thread-local identity cache hints.
- MyLite MariaDB handler direct-update and exact-index read paths.

## Compatibility Impact

No SQL-visible behavior change is intended. The combined scope stores and
restores the same thread-local identity values as the two separate scopes, only
through one paired API.

## Single-File And Embedded Lifecycle Impact

No file-format, journal, lock, sidecar, recovery, or embedded lifecycle change.
The scope remains thread-local and active only for the same handler call
windows as before.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change. The added storage
scope is an internal first-party storage API used by the bundled handler.

## Binary-Size And Dependency Impact

No dependency change. Binary-size impact is limited to two small storage
functions and one handler RAII wrapper.

## Verification

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/include/mylite/storage.h
  mariadb/storage/mylite/ha_mylite.cc` passed.
- `cmake --build build/mariadb-mylite-storage-smoke --target
  libmariadbd.a` passed; resulting archive size is 21,275,440 bytes.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_embedded_statement_test
  mylite_perf_baseline` passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine|libmylite.embedded-statement'
  --output-on-failure` passed 3/3.
- `ctest --preset storage-smoke-dev --output-on-failure` passed 10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000` measured the
  prepared row-only update step at 1.566 us/op.
- A 20M-iteration sampled run with
  `--phase=prepared-row-only-update-components --profile-iterations=20000000
  10000` measured the prepared row-only update step at 1.519 us/op. The sampled
  hot-path summary shows the combined scope calls and no separate table-name
  identity-scope calls on the direct-update path; filename-only scope calls
  remain for unchanged filename-only callers.

## Acceptance Criteria

- Paired handler paths call one combined identity-scope begin/end pair instead
  of separate filename and table-name begin/end pairs.
- Filename-only callers keep existing behavior.
- Existing storage-smoke tests pass.
- Local prepared row-only update timing is stable or faster.

## Risks And Unresolved Questions

- This is a small control-plane cleanup, not the broader prepared-DML rebind
  work needed to remove `JOIN::prepare()` from the hot path.
