# Guard Empty Index Cursor Frees

## Problem

Prepared primary-key updates rebuild a handler index cursor for each point
lookup. `ha_mylite::build_index_cursor()` starts by clearing the existing
cursor. When the cursor is already empty, `clear_index_cursor()` still calls
`mylite_storage_free(NULL)` for each nullable cursor buffer whose inline
sentinel differs from `NULL`.

Those calls are legal, but the sampled prepared-update profile shows
`mylite_storage_free()` in handler cursor setup.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  the first-party MyLite handler.
- `ha_mylite::build_index_cursor()` calls `clear_index_cursor()` before
  populating a new cursor.
- The exact unique primary-key path uses inline cursor storage for key, entry,
  row offset, row size, and row payload scratch.
- `clear_index_cursor()` compares nullable pointers only against inline
  sentinels, so `NULL` values still flow through `mylite_storage_free()`.

## Design

- Add explicit non-null guards to each free in `clear_index_cursor()`.
- Preserve the existing inline-storage checks so inline buffers are never
  released.
- Leave cursor state reset behavior unchanged.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. The handler releases the same owned buffers and resets the
same cursor state.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.

## Tests And Verification

- Run:
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
- Run prepared-update timing only when unrelated machine load is low.

## Acceptance Criteria

- Empty index cursor cleanup does not call the storage free wrapper with
  `NULL` cursor pointers.
- Existing routed storage, cursor, transaction, rollback, and embedded
  storage-engine tests pass.

## Risks

- The guard must not skip owned non-inline buffers. Each condition keeps both
  ownership checks: non-null and not the corresponding inline sentinel.
