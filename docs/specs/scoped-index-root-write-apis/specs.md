# Scoped Index-Root Write APIs

## Problem

Index-root metadata reads now use scoped file/header setup, but the catalog
write helpers that publish or remove those root records still use the older
update wrapper:

- `mylite_storage_store_index_root()`
- `mylite_storage_drop_index_root()`

These helpers mutate only catalog root metadata and should follow the same
`mylite_storage_update_file_scope` ownership model as schema namespace writes.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- `mylite_storage_read_index_root()` already uses scoped read setup and the
  active index-root catalog-entry cache.
- `mylite_storage_store_index_root()` and
  `mylite_storage_drop_index_root()` still use
  `open_existing_file_for_update()`, `read_header()`, and
  `close_existing_file()`.
- `open_existing_file_for_update_scope()` and
  `read_header_from_update_file_scope()` preserve active write/read statement
  header visibility and lock ownership.

## Scope

- Move the listed index-root write helpers to
  `open_existing_file_for_update_scope()`.
- Use `read_header_from_update_file_scope()`.
- Close through `close_existing_update_file_scope()`.
- Preserve root-page validation, catalog record replacement/removal, recovery
  journal publication, and error behavior.

## Non-Goals

- No SQL behavior change.
- No public C API change.
- No file-format change.
- No index leaf rebuild conversion.
- No maintained-root page mutation or B-tree navigation work.

## Design

Each helper opens a `mylite_storage_update_file_scope`, reads the scoped header,
and runs the existing catalog-image mutation logic. Publication still creates
the recovery journal and writes the catalog image with the same page-count and
root-record semantics as before.

## Compatibility Impact

SQL-visible behavior is unchanged. Existing index-root metadata publication,
rename/drop cleanup, and maintained-root entry-count reads continue to use the
same catalog records.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. Existing recovery
journals remain the only transient companion involved in catalog publication.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite index-root metadata writes benefit. Runtime-volatile MEMORY/HEAP
row behavior is unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
update-scope helpers.

## Test Plan

- Re-run storage unit coverage for index-root metadata store/drop/rename/drop
  cleanup, maintained-root entry counts, locking, rollback, and recovery.
- Re-run storage-engine smoke because routed DDL and index publication paths
  use catalog-backed root metadata.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- The listed index-root write helpers use scoped update file/header setup.
- Existing root metadata publication and cleanup behavior remains covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- Index leaf rebuilds and maintained-root page writes still need separate
  update-scope work because they write data pages as well as catalog metadata.

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
