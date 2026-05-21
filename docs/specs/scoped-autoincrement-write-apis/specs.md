# Scoped Autoincrement Write APIs

## Problem

Durable autoincrement reads now use scoped file/header setup, but the scalar
write helpers still use the older update wrapper:

- `mylite_storage_set_auto_increment()`
- `mylite_storage_advance_auto_increment()`

These helpers should use `mylite_storage_update_file_scope` and the active
table-entry cache while preserving their no-op and publication semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- `mylite_storage_read_auto_increment()` already uses scoped read setup and
  `find_table_id_in_statement()`.
- The autoincrement write helpers still use `open_existing_file_for_update()`,
  `read_header()`, `find_table_id()`, and `close_existing_file()`.
- `advance_auto_increment()` returns success without publication when the
  requested value is not greater than the current value, and that no-op path
  must close the update scope correctly.

## Scope

- Move the listed autoincrement write helpers to
  `open_existing_file_for_update_scope()`.
- Use `read_header_from_update_file_scope()`.
- Resolve the table id through `find_table_id_in_statement()` when an active
  cache statement is available.
- Close through `close_existing_update_file_scope()`.
- Preserve current value comparison, publication, no-op success, cleanup, and
  error behavior.

## Non-Goals

- No SQL behavior change.
- No public C API change.
- No file-format change.
- No allocator, row-insert reservation, or rollback gap semantics change.
- No row write conversion.

## Design

Each helper opens a `mylite_storage_update_file_scope`, reads the scoped header,
resolves the table id through the active table-entry cache when possible, reads
the current durable autoincrement value, and publishes only when the existing
conditions require publication. The `advance_auto_increment()` no-op path
closes the update scope before returning success.

## Compatibility Impact

SQL-visible autoincrement behavior is unchanged. Existing ALTER
`AUTO_INCREMENT`, explicit high-value advancement, no-op lower set/advance
paths, and rollback behavior remain covered by current tests.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. Existing recovery
journals remain the only transient companion involved when publication occurs.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite autoincrement metadata writes benefit. Runtime-volatile
MEMORY/HEAP autoincrement state is unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
update-scope and table-entry cache helpers.

## Test Plan

- Re-run storage unit coverage for autoincrement set/advance, no-op paths,
  table-local allocation, rollback, locking, and recovery.
- Re-run storage-engine smoke because routed DDL/DML autoincrement paths use
  these helpers.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- The listed autoincrement write helpers use scoped update file/header setup.
- The no-op `advance_auto_increment()` path closes through the update scope.
- Existing autoincrement behavior remains covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- Row insert, update, delete, and truncate paths still need separate
  update-scope slices because they protect row/index pages and caches.

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
