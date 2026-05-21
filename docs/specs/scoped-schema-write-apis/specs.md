# Scoped Schema Write APIs

## Problem

Read-only storage APIs now use explicit file scopes, but schema namespace
catalog writes still use the older update wrapper:

- `mylite_storage_store_schema()`
- `mylite_storage_store_schema_definition()`
- `mylite_storage_drop_schema()`

These helpers mutate only catalog namespace records and should use
`mylite_storage_update_file_scope` so active statements, read statements, and
transaction snapshots share the same update file/header ownership rules as
newer storage paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- `open_existing_file_for_update_scope()` already handles active write
  statements, lock upgrades for same-owner read statements, and busy rejection
  when another owner has an active statement.
- `read_header_from_update_file_scope()` preserves active write and read
  statement header visibility.
- `close_existing_update_file_scope()` releases borrowed statement files
  without closing statement-owned handles.

## Scope

- Move the listed schema write helpers to
  `open_existing_file_for_update_scope()`.
- Use `read_header_from_update_file_scope()`.
- Close through `close_existing_update_file_scope()`.
- Preserve the existing `store_schema()` no-op success path when the schema
  already exists.
- Preserve current catalog publication, journal creation, cleanup, and error
  behavior.

## Non-Goals

- No SQL behavior change.
- No public C API change.
- No file-format change.
- No table, index, FK, row, or autoincrement write conversion.
- No locking redesign beyond using the existing update-scope helper.

## Design

Each schema helper opens a `mylite_storage_update_file_scope`, reads the scoped
header, and runs the existing catalog-image mutation logic. The no-op
`store_schema()` path closes the update scope before returning success. The
publishing paths continue to create the recovery journal and publish the
catalog image exactly as before.

## Compatibility Impact

SQL-visible schema DDL behavior is unchanged. Existing duplicate-schema,
schema-definition replacement, and drop behavior remains intact.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. The helpers continue to
use the existing recovery journal path when catalog publication is needed.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

MyLite-routed schema namespace writes benefit. Table-level routing and
runtime-volatile MEMORY/HEAP rows are unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
update-scope helpers.

## Test Plan

- Re-run storage unit coverage for schema store, schema definition,
  duplicate/no-op create, drop, catalog rollback, locking, and recovery.
- Re-run storage-engine smoke because SQL-layer `CREATE DATABASE` and
  namespace discovery exercise these paths.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- The listed schema write helpers use scoped update file/header setup.
- The existing no-op schema-create success path closes through the update
  scope.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- Table, FK, index-root, row, and autoincrement mutation helpers still need
  separate update-scope slices because their payload and journal semantics are
  broader than schema catalog records.

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
