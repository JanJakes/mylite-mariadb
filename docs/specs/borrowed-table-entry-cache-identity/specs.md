# Borrowed Table-Entry Cache Identity

## Problem

Prepared point updates repeatedly resolve the same schema/table pair through
the active statement table-entry cache. The cache stores owned copies of the
schema and table names, so pointer equality against the caller's stable name
buffers almost never succeeds and the hot cache-hit path still pays
`strcmp()`.

The current behavior is correct, but it keeps name comparison work in a path
that is otherwise dominated by row lookup and update execution.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::index_read_map()` routes
  full-key exact unique reads to
  `ha_mylite::read_exact_unique_index_row_into()` before building a generic
  cursor.
- `ha_mylite::read_exact_unique_index_row_into()` passes stable handler
  `storage_schema()` and `storage_table()` names into
  `mylite_storage_find_indexed_row_into()`.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::update_row()` passes
  schema/table names into the MyLite storage row-update variants for
  successful routed row DML.
- `packages/mylite-storage/src/storage.c:find_table_id_in_statement()` uses
  `find_active_table_entry_cache_in_statement()` before materializing the
  catalog image.
- `find_active_table_entry_cache_in_statement()` currently compares the owned
  cached name copies with the caller's name buffers, falling through to
  `strcmp()` when the pointers differ.
- A macOS `sample` run of
  `mylite_perf_baseline --phase=prepared-updates 1000 1000000` after the
  same-row exact-cache replacement slice still showed `_platform_strcmp` under
  `find_table_id_in_statement()` and exact-index lookup frames.

## Design

- Keep the existing owned `schema_name` and `table_name` copies in
  `mylite_storage_table_entry_cache`. They remain the authoritative fallback
  comparison data and preserve the cache across caller buffer lifetime changes.
- Add borrowed `const char *` identity fields beside the owned copies.
- Add an explicit table-name identity scope for callers that can prove their
  schema/table buffers are stable for the scope. The MyLite handler can make
  that promise for `storage_schema()` and `storage_table()` while the handler
  instance is open.
- On cache store, record the caller's current schema/table pointers in the
  identity fields only when the explicit stable-name scope is active for those
  exact pointers.
- On cache hit, compare borrowed identities before falling back to `strcmp()`
  against the owned copies only when the current stable-name scope matches the
  cached identity.
- When updating an existing cache entry for the same logical table, refresh the
  borrowed identities to the latest caller buffers.
- Never own, free, or dereference the borrowed identities for anything other
  than pointer equality. The owned copies continue to guard correctness.

## Scope

In scope:

- Active statement table-entry cache lookup and store paths in first-party
  MyLite storage code.
- Handler exact unique reads and row updates where `storage_schema()` and
  `storage_table()` are stable for the handler lifetime.
- Prepared-update performance evidence for routed durable storage.

Out of scope:

- Index-root cache name identity shortcuts.
- SQL planner changes, handler API changes, public C API changes, and file
  format changes.
- Broader schema/table name interning.

## Compatibility Impact

No SQL, storage-engine routing, `libmylite` public C API, or catalog behavior
changes. The first-party storage package gets an opt-in table-name identity
scope for trusted callers. Name equality remains byte-for-byte through the
owned-copy fallback when pointer identity is not explicitly trusted.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only affects transient active statement cache memory.

## Binary-Size And Dependency Impact

Small first-party C helper and two pointer fields in a transient cache struct.
No new dependencies.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run the storage unit test and focused routed-storage CTest coverage.
- Add storage coverage for raw callers that reuse the same mutable schema/table
  buffers for different tables without entering a stable-name scope.
- Run the full `storage-smoke-dev` CTest preset.
- Run `git diff --check` and `git clang-format --diff` on touched C files.
- Run the prepared-update performance baseline and sample it to confirm the
  table-entry cache `strcmp()` frames are reduced or gone on stable-name hot
  paths.

## Acceptance Criteria

- Active table-entry cache hits match by borrowed pointer identity when the
  caller reuses the same schema/table buffers.
- Cache hits still match by owned string content when caller pointers differ.
- Raw storage callers that reuse one mutable name buffer for different table
  names still match by owned string content unless they explicitly opt into a
  stable-name scope.
- Cache clear and replacement paths free only owned strings and leave no stale
  owned memory behind.
- Existing storage and routed embedded tests pass.

## Risks

- Borrowed identity pointers may outlive the caller buffers. This is acceptable
  only because the implementation uses them for equality comparison, not for
  dereference or ownership, and only trusts them while a caller-provided
  stable-name scope is active.
- The optimization is intentionally narrow. If future samples show
  `strcmp()` under index-root cache lookups instead, that should be a separate
  measured slice.

## Verification Evidence

- `git diff --check`: passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/include/mylite/storage.h
  mariadb/storage/mylite/ha_mylite.cc
  packages/mylite-storage/tests/storage_test.c`: no changes.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed during
  the follow-up active-write exact-read scope work, rebuilding
  `mariadb/storage/mylite/ha_mylite.cc` and the MyLite storage archive objects
  with this slice included.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  2/2 passed.
- `ctest --preset storage-smoke-dev --output-on-failure`: 10/10 passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key updates
  measured `2.326 us/op`; repeated follow-up runs measured `2.309`,
  `2.276`, and `2.258 us/op`.
- A one-second macOS `sample` run showed remaining `find_table_id_in_statement`
  frames, but table-name `strcmp()` samples under that path dropped to a small
  residual count after scoped hits refresh the borrowed identities.
