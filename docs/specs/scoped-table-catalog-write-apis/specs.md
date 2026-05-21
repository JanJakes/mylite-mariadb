# Scoped Table Catalog Write APIs

## Problem

Several table-level catalog writers still use the older update wrapper even
though schema, FK, index-root, and autoincrement metadata writes now use update
file scopes:

- `mylite_storage_store_table_definition()`
- `mylite_storage_drop_table()`
- `rename_table()` through public table rename entry points

These helpers publish table catalog records, definition BLOB pages, or table
identity changes and should use `mylite_storage_update_file_scope`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- Read-side table metadata APIs already use scoped file/header setup.
- Schema, FK, index-root, and autoincrement write helpers now use
  `open_existing_file_for_update_scope()` and
  `read_header_from_update_file_scope()`.
- The table catalog writers listed above still use
  `open_existing_file_for_update()`, `read_header()`, and
  `close_existing_file()`.

## Scope

- Move the listed table catalog writers to
  `open_existing_file_for_update_scope()`.
- Use `read_header_from_update_file_scope()`.
- Close through `close_existing_update_file_scope()`.
- Preserve table definition BLOB publication, duplicate detection, FK-aware
  drop cleanup/rejection, rename semantics, recovery journal publication, and
  cleanup behavior.

## Non-Goals

- No SQL behavior change.
- No public C API change.
- No file-format change.
- No row append/update/delete/truncate conversion.
- No index leaf rebuild conversion.

## Design

Each table catalog writer opens a `mylite_storage_update_file_scope`, reads the
scoped header, and runs the existing catalog/BLOB mutation logic unchanged.
`store_table_definition()` still writes definition BLOB pages before publishing
the catalog image. `drop_table()` still removes child FK metadata and index
root records while rejecting parent-referenced drops. `rename_table()` keeps
its existing `preserve_foreign_keys` behavior for rebuild backup renames.

## Compatibility Impact

SQL-visible table DDL behavior is unchanged. Existing create, drop, rename,
LIKE/CTAS, FK cleanup, parent-drop rejection, and rollback behavior remain
covered by current storage and storage-smoke tests.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. Existing recovery
journals remain the only transient companion involved in table catalog
publication.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite table catalog writes benefit. Runtime-volatile MEMORY/HEAP row
storage is unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
update-scope helpers.

## Test Plan

- Re-run storage unit coverage for table definition store/read, duplicate
  create, drop, rename, FK-aware drop cleanup/rejection, locking, rollback, and
  recovery.
- Re-run storage-engine smoke because routed CREATE/DROP/RENAME, LIKE/CTAS,
  and FK table metadata paths exercise these helpers.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- The listed table catalog writers use scoped update file/header setup.
- Existing table catalog, definition BLOB, FK cleanup, and rename behavior
  remains covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- Row mutation and index leaf rebuild paths still need separate update-scope
  slices because they write row/index pages and maintain active caches.

## Verification Results

Recorded 2026-05-21:

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  reported no changes.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  libmylite.embedded-storage-engine --output-on-failure` passed.
