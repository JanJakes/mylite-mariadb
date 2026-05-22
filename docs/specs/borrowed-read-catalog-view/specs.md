# Borrowed Read Catalog View

## Problem

Durable point reads start a storage read statement that already owns a decoded
catalog image for the current header/catalog generation. The exact indexed row
lookup path then calls `read_catalog_image()`, which copies that cached catalog
image into a temporary allocation before scanning it for the table record and
index-root record.

That copy is correct, but it is avoidable for read-only helpers that only need
to inspect catalog records during one storage call. Prepared primary-key point
selects repeat this path for every routed lookup.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/mylite/ha_mylite.cc::read_exact_unique_index_row_into()`
  wraps durable exact row lookup in `Mylite_read_statement_scope`.
- `packages/mylite-storage/src/storage.c::initialize_read_statement()` reads
  and stores `statement->current_catalog_image` for the active read statement.
- `read_catalog_image()` first checks active statement/read-statement cached
  catalog images, but returns a deep copy through
  `copy_cached_catalog_image_from_statement()`.
- `find_indexed_row_payload()` and `mylite_storage_find_index_entry()` scan the
  catalog image without mutating it before calling `find_exact_index_row_id()`.
- `find_exact_index_row_id()` only needs a catalog image for leaf/root lookup
  after exact caches have missed.

## Design

- Add a storage-local helper that borrows a current catalog image from the
  active write statement, read statement, or read snapshot when its catalog root
  and generation match the supplied header.
- Add a `catalog_image_view_for_file()` helper that returns a borrowed view when
  available and falls back to `read_catalog_image()` into caller-owned storage.
- Use that helper in the exact indexed row lookup path so table and index-root
  record scans can inspect the active read statement's catalog image without
  allocating and copying it.
- Keep `read_catalog_image()` unchanged for callers that need an owned catalog
  image.
- Keep fallback ownership explicit: only the locally owned catalog image is
  freed; borrowed statement-owned images remain owned by their statement.

## Compatibility Impact

No SQL-visible behavior should change. The borrowed catalog view is used only
when it matches the same header catalog root and generation that the owned copy
would have been made from.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing-policy change. Durable routed tables can avoid a transient catalog
copy during exact indexed reads; volatile MEMORY/HEAP rows are unaffected.

## Binary-Size And Dependency Impact

Small first-party storage helper additions. No dependency change.

## Tests And Verification

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - `prepared primary-key point selects`: `9.021 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-scalar-selects 10000 1000000`
  - `prepared scalar selects`: `0.713 us/op`

## Acceptance Criteria

- Exact indexed row lookup uses a borrowed active read-statement catalog image
  when available.
- Callers that need an owned catalog image still use `read_catalog_image()`.
- Owned fallback catalog images are still freed exactly once.
- Existing storage and storage-engine smoke tests pass.
- Local benchmark evidence records routed point-select behavior.

## Risks And Unresolved Questions

- Borrowed catalog pointers must not escape the storage call. The helper keeps
  the view local and does not store it.
- This removes a transient copy but still scans catalog records. Broader catalog
  lookup acceleration or persistent table/root record handles remain separate
  work.
