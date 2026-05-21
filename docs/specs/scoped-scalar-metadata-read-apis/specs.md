# Scoped Scalar Metadata Read APIs

## Problem

Most hot row and catalog read paths now use `mylite_storage_file_scope` and
scoped header reads when an active statement, read statement, or snapshot
already owns a stable file view. A few scalar metadata helpers still use the
older open/header/close sequence:

- `mylite_storage_open_header()`
- `mylite_storage_read_index_root()`
- `mylite_storage_read_auto_increment()`

Those helpers should reuse the same scoped file and header state as the newer
row, catalog, and index-entryset reads.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- `open_existing_file_scope()` already returns borrowed active-statement,
  active-read-statement, and active-read-snapshot files without transferring
  ownership.
- `read_header_from_file_scope()` mirrors the visibility rules from
  `read_header()`, including active checkpoint headers, read statements,
  read snapshots, and transaction journal snapshots.
- `find_table_id_in_statement()` and
  `find_index_root_record_in_statement()` can reuse active table and
  index-root catalog entry caches after the scoped header identifies the same
  catalog root page and generation.

## Scope

- Move the listed scalar metadata read APIs to `open_existing_file_scope()`.
- Use `read_header_from_file_scope()`.
- Close through `close_existing_file_scope()`.
- Route table-id and index-root catalog lookups through the active statement
  caches when a scoped active statement is available.
- Preserve existing validation, metadata reset, autoincrement, and error
  behavior.

## Non-Goals

- No SQL behavior change.
- No public C API change.
- No file-format change.
- No new catalog lookup structure.
- No write-side metadata or autoincrement mutation conversion.
- No B-tree, pager, WAL, or locking redesign.

## Design

Each target helper opens a `mylite_storage_file_scope`, derives the borrowed or
owned `FILE *`, and reads the scoped header. `mylite_storage_open_header()`
keeps the existing catalog-root and free-list validation after the scoped
header read. `mylite_storage_read_index_root()` reads the catalog image from
the scoped view, resolves the table record, then resolves the index-root record
through the active index-root cache. `mylite_storage_read_auto_increment()`
resolves the table id through the active table-entry cache before reading the
latest autoincrement value.

## Compatibility Impact

SQL-visible behavior is unchanged. The same checkpoint view selected by the
storage file scope is used for scalar metadata reads, matching the row and
catalog read APIs already converted to scoped setup.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. Borrowed statement
files are released through the scope helper instead of the raw file closer.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite header validation, index-root metadata, and autoincrement reads
benefit. Runtime-volatile MEMORY/HEAP row behavior is unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
scope and cache helpers.

## Test Plan

- Re-run storage unit coverage for header reads, active-header visibility,
  index-root metadata, autoincrement, catalog caches, locking, rollback, and
  recovery.
- Re-run storage-engine smoke because routed handlers use these metadata paths
  while opening and executing tables.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- The listed scalar metadata read APIs use scoped file/header setup.
- Existing active-header, index-root metadata, autoincrement, and error
  behavior remains covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- Remaining write-side helpers still use update-oriented open/header paths and
  should be converted only in slices that preserve their locking and journal
  semantics.

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
