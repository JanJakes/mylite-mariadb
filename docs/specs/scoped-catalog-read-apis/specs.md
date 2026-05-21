# Scoped Catalog Read APIs

## Problem

MariaDB discovery callbacks can already run inside scoped MyLite read sessions,
but several read-only catalog APIs still open the primary file and read the
header through the older generic path. That leaves catalog discovery and
metadata reads unable to reuse scoped file/header state when they are called
inside an active statement, read statement, or snapshot.

The immediate targets are read-only schema/table discovery helpers:

- `mylite_storage_schema_exists()`
- `mylite_storage_read_schema_definition()`
- `mylite_storage_read_table_definition()`
- `mylite_storage_read_table_metadata()`
- `mylite_storage_table_exists()`
- `mylite_storage_list_foreign_keys()`
- `mylite_storage_list_parent_foreign_keys()`
- `mylite_storage_list_tables()`
- `mylite_storage_list_schemas()`

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc` discovery callbacks call storage
  helpers including table definition, table listing, and table existence.
- The existing `docs/specs/discovery-read-session/specs.md` slice scoped the
  handler callbacks, but the storage helpers listed above still use
  `open_existing_file()`, generic `read_header()`, and `close_existing_file()`.
- `read_catalog_image()` can already reuse a cached catalog image for active
  statements and read statements after a scoped header identifies the same
  catalog root page and generation.

## Scope

- Move the listed catalog read APIs to `open_existing_file_scope()`.
- Use `read_header_from_file_scope()`.
- Close through `close_existing_file_scope()`.
- Preserve current catalog image reads, BLOB metadata reads, allocation
  cleanup, and error behavior.

## Non-Goals

- No SQL behavior change.
- No file-format change.
- No public API change.
- No catalog lookup index or record-level cache.
- No write-side metadata DDL conversion.

## Design

Each target helper opens a `mylite_storage_file_scope`, reads the scoped header,
then executes its existing catalog-image and record decode logic unchanged.
The helpers continue to return owned definition or metadata memory through the
same public cleanup APIs.

## Compatibility Impact

SQL-visible behavior is unchanged. The same catalog records and BLOB pages are
decoded from the checkpoint view selected by the storage file scope.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. Borrowed statement
files are released through the scope helper instead of the raw file closer.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite discovery and metadata paths benefit. Runtime-volatile
MEMORY/HEAP rows remain unchanged after catalog discovery.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
scope helpers.

## Test Plan

- Re-run storage unit coverage for schema/table metadata, table listing,
  discovery, reopened catalog metadata, catalog-image cache, locking, rollback,
  and recovery.
- Re-run storage-engine smoke because MariaDB table discovery calls these APIs.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- The listed catalog read APIs use scoped file/header setup.
- Existing catalog metadata, definition, listing, and discovery behavior remains
  covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- Foreign-key definition reads and write-side catalog mutations remain on
  separate follow-up slices because they have different BLOB decode and update
  behavior.

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
