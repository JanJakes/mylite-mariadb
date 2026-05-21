# Scoped Row Mutation Write APIs

## Problem

Most storage reads and metadata writes now use explicit file scopes, but the
remaining row mutation helpers still use the older generic update wrapper:

- `mylite_storage_append_row_with_index_entries()`
- `mylite_storage_delete_row()`
- `mylite_storage_truncate_table()`

Those helpers write row pages, row-state pages, maintained index roots,
autoincrement pages, and active/durable caches. They should use
`mylite_storage_update_file_scope` while preserving the existing mutation and
publication semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- `update_row_with_index_entries()` already uses update file scopes and is the
  local pattern for row mutation ownership, scoped header reads, and active
  statement close handling.
- The append, delete, and truncate helpers still use
  `open_existing_file_for_update()`, `read_header()`, and
  `close_existing_file()`.
- After these helpers move to scoped setup, the generic
  `open_existing_file_for_update()` wrapper has no remaining callers.

## Scope

- Move the listed row mutation helpers to
  `open_existing_file_for_update_scope()`.
- Use `read_header_from_update_file_scope()`.
- Close through `close_existing_update_file_scope()`.
- Resolve truncate table ids through `find_table_id_in_statement()` when an
  active cache statement is available.
- Remove the now-unused generic update-open wrapper.
- Remove the now-unused generic `find_table_id()` wrapper.
- Remove the now-unused filename-based active table-entry cache wrapper.
- Preserve row page writes, row-state publication, maintained-index plans,
  autoincrement reset, active/durable cache updates, recovery journal
  publication, and cleanup behavior.

## Non-Goals

- No SQL behavior change.
- No public C API change.
- No file-format change.
- No B-tree split/merge or pager redesign.
- No new row/index cache behavior.

## Design

Append, delete, and truncate open a `mylite_storage_update_file_scope`, read the
scoped header, and then run their existing mutation logic. Append and delete
reuse the scoped active statement when beginning statement journals and
retargeting durable caches. Truncate resolves the table id through the active
table-entry cache where possible before scanning live rows and publishing
truncate row-state/autoincrement pages.

## Compatibility Impact

SQL-visible row behavior is unchanged. Existing insert, delete, truncate,
maintained-root mutation, autoincrement reset, rollback, and recovery behavior
remain covered by storage and storage-smoke tests.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. Existing recovery and
transaction journals remain the only transient companions involved in row
mutation publication.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite row mutation paths benefit. Runtime-volatile MEMORY/HEAP row
storage is unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Removing the unused generic update wrapper is
a tiny first-party cleanup.

## Test Plan

- Re-run storage unit coverage for insert, indexed insert, delete, truncate,
  maintained roots, autoincrement reset, active caches, locking, rollback, and
  recovery.
- Re-run storage-engine smoke because routed DML, FK checks, truncate, and
  copy-rebuild flows exercise these paths.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- The listed row mutation helpers use scoped update file/header setup.
- The generic update-open wrapper is removed.
- The generic table-id wrapper is removed after remaining callers move to
  `find_table_id_in_statement()`.
- The unused filename-based active table-entry cache wrapper is removed.
- Existing row mutation, cache, rollback, and recovery behavior remains
  covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- This is a lifecycle/ownership cleanup, not a B-tree or pager performance
  replacement. SQLite-like performance still depends on navigable indexes and
  broader pager work.

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
