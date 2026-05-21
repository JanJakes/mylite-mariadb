# Scoped Foreign-Key Write APIs

## Problem

Foreign-key definition reads now use scoped file/header setup, but the write
helpers that publish, update, or remove FK metadata still use the older update
wrapper:

- `mylite_storage_store_foreign_key_definition()`
- `mylite_storage_update_foreign_key_referenced_key_name()`
- `mylite_storage_drop_foreign_key_definition()`

These helpers should use `mylite_storage_update_file_scope` while preserving
their FK metadata BLOB and catalog publication semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- `mylite_storage_read_foreign_key_definition()` already uses scoped read setup
  before reading FK catalog records and metadata BLOB pages.
- The FK write helpers still use `open_existing_file_for_update()`,
  `read_header()`, and `close_existing_file()`.
- `read_header_from_update_file_scope()` preserves active write and read
  statement header visibility, and `close_existing_update_file_scope()` handles
  borrowed statement files.

## Scope

- Move the listed FK write helpers to `open_existing_file_for_update_scope()`.
- Use `read_header_from_update_file_scope()`.
- Close through `close_existing_update_file_scope()`.
- Preserve metadata encode/decode, BLOB page writes, catalog record
  replacement/removal, recovery journal publication, and cleanup behavior.

## Non-Goals

- No SQL behavior change.
- No public C API change.
- No file-format change.
- No FK action, validation, or row-check semantics change.
- No table or row mutation conversion.

## Design

Each FK helper opens a `mylite_storage_update_file_scope`, reads the scoped
header, and then runs the existing FK catalog/BLOB mutation logic unchanged.
`update_foreign_key_referenced_key_name()` keeps the old metadata and decoded
metadata alive until the replacement record and BLOB pages are ready.

## Compatibility Impact

SQL-visible FK DDL behavior is unchanged. Existing FK metadata publication,
referenced-key rename metadata updates, drop behavior, and handler metadata
publication remain backed by the same catalog records and BLOB pages.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. Existing recovery
journals remain the only transient companion involved in FK catalog
publication.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite FK metadata writes benefit. Runtime-volatile MEMORY/HEAP rows
are unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
update-scope helpers.

## Test Plan

- Re-run storage unit coverage for FK store/read/list/parent-list/drop,
  referenced-key rename metadata updates, locking, rollback, and recovery.
- Re-run storage-engine smoke because routed FK DDL, `SHOW CREATE TABLE`, and
  handler metadata hooks exercise these paths.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- The listed FK write helpers use scoped update file/header setup.
- Existing FK metadata BLOB publication and cleanup behavior remains covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- Table-definition and table-rename/drop writers remain separate because they
  mix table catalog records with FK cleanup and definition BLOB metadata.

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
