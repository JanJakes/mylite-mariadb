# Scoped Index Leaf Rebuild Write

## Problem

`mylite_storage_rebuild_index_leaves()` still uses the older update wrapper
while rebuilding published index leaf pages and catalog root records. The
remaining metadata writers now use `mylite_storage_update_file_scope`; the leaf
rebuild path should follow the same file/header ownership model while keeping
its page-writing behavior unchanged.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- `mylite_storage_rebuild_index_leaves()` reads live index entrysets, prepares
  rebuilt leaf pages, publishes root metadata into the catalog, writes pages
  through the pager, and then publishes the catalog image.
- The helper still uses `open_existing_file_for_update()`, `read_header()`,
  and `close_existing_file()`.

## Scope

- Move `mylite_storage_rebuild_index_leaves()` to
  `open_existing_file_for_update_scope()`.
- Use `read_header_from_update_file_scope()`.
- Close through `close_existing_update_file_scope()`.
- Preserve rebuild allocation cleanup, live entryset reads, leaf-page writes,
  root metadata publication, recovery journal publication, and error behavior.

## Non-Goals

- No SQL behavior change.
- No public C API change.
- No file-format change.
- No B-tree split/merge or multi-page maintained-root mutation.
- No row insert/update/delete/truncate conversion.

## Design

The rebuild helper opens a `mylite_storage_update_file_scope`, reads the scoped
header, and then runs the existing rebuild plan, page write, and catalog
publication logic unchanged. Failure before opening still frees the rebuild
plan exactly as before; failure after opening closes through the update scope.

## Compatibility Impact

SQL-visible behavior is unchanged. Existing copy-rebuild DDL and published leaf
root behavior remain covered by storage and storage-smoke tests.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. Existing recovery
journals remain the only transient companion involved in leaf-root
publication.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite rebuilt leaf publication benefits. Runtime-volatile MEMORY/HEAP
rows are unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
update-scope helpers.

## Test Plan

- Re-run storage unit coverage for published leaf rebuilds, maintained roots,
  copy-rebuild DDL, locking, rollback, and recovery.
- Re-run storage-engine smoke because routed copy-rebuild DDL can publish leaf
  roots.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- `mylite_storage_rebuild_index_leaves()` uses scoped update file/header setup.
- Existing published leaf rebuild behavior remains covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- Row mutation paths remain separate because they protect maintained pages,
  update row/index caches, and publish row-state pages.

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
