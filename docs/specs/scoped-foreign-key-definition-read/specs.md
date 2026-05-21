# Scoped Foreign-Key Definition Read

## Problem

Catalog listing and discovery reads now use `mylite_storage_file_scope`, but
`mylite_storage_read_foreign_key_definition()` still opens the primary file and
reads the header through the older generic path. This helper decodes
catalog-backed FK BLOB metadata, so it was left out of the first catalog-read
cleanup and needs its own scoped conversion.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- `mylite_storage_list_foreign_keys()` and
  `mylite_storage_list_parent_foreign_keys()` already use scoped file/header
  reads before scanning FK catalog records.
- `mylite_storage_read_foreign_key_definition()` still uses
  `open_existing_file()`, `read_header()`, and `close_existing_file()` before
  reading FK metadata BLOB pages and decoding the record.

## Scope

- Move `mylite_storage_read_foreign_key_definition()` to
  `open_existing_file_scope()`.
- Use `read_header_from_file_scope()`.
- Close through `close_existing_file_scope()`.
- Preserve current catalog lookup, FK metadata BLOB decoding, owned memory
  cleanup, and error behavior.

## Non-Goals

- No SQL behavior change.
- No public C API change.
- No file-format change.
- No FK metadata layout change.
- No write-side FK metadata mutation conversion.

## Design

The helper opens a `mylite_storage_file_scope`, reads the scoped header, and
then executes the existing catalog-image lookup, BLOB-page read, and metadata
decode logic unchanged. The catalog image remains alive until
`decode_foreign_key_metadata()` has used the record pointer.

## Compatibility Impact

SQL-visible behavior is unchanged. Handler FK metadata publication and
`SHOW CREATE TABLE` use the same FK catalog records, but can now share scoped
file/header state when called inside an active or read statement view.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. Borrowed statement
files are released through the scope helper instead of the raw file closer.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite FK metadata reads benefit. Runtime-volatile MEMORY/HEAP row
behavior is unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
scope helpers.

## Test Plan

- Re-run storage unit coverage for FK metadata store/read/list, catalog caches,
  locking, rollback, and recovery.
- Re-run storage-engine smoke because handler metadata hooks expose FK
  definitions through MariaDB table metadata paths.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- `mylite_storage_read_foreign_key_definition()` uses scoped file/header setup.
- Existing FK metadata read/list and handler publication behavior remains
  covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- FK metadata mutation helpers remain on write-oriented open/header paths and
  need separate update-scope work to preserve journal semantics.

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
