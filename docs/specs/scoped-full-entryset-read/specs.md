# Scoped Full Entryset Read

## Problem

`mylite_storage_read_index_entries()` builds the full live entryset for one
index. Handler ordered index cursor construction still depends on this path
when exact/static shortcuts do not apply. The helper still uses
`open_existing_file()` and generic `read_header()`, even though adjacent exact
entryset and prefix-exists helpers now use scoped file/header reads.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::mylite_storage_read_index_entries()`
  opens through `open_existing_file()` and reads the header through
  `read_header()`.
- `mylite_storage_read_exact_index_entries()` and
  `mylite_storage_index_prefix_exists_for_index()` use
  `mylite_storage_file_scope`, scoped header reads, and active table-entry
  cache reuse.

## Scope

- Move `mylite_storage_read_index_entries()` onto `mylite_storage_file_scope`.
- Use `read_header_from_file_scope()`.
- Reuse the active table-entry cache when available.
- Preserve published-root full reads and append-history fallback behavior.

## Non-Goals

- No SQL behavior change.
- No file-format change.
- No public API change.
- No new index-page format, B-tree split, merge, or root-cache work.

## Design

Use the same setup pattern as exact entryset reads:

1. Open a scoped file view.
2. Read the current header from the scope.
3. Try the active table-entry cache.
4. Read the catalog image as needed for root discovery and fallback.
5. Close through the scoped close helper.

The body after table/root discovery remains unchanged: static roots feed the
full-entryset leaf path, and missing roots fall back to append-history scans.

## Compatibility Impact

No SQL-visible change. Full index-entryset reads preserve row visibility and
ordering semantics.

## Single-File And Lifecycle Impact

No lifecycle change. The helper continues to read one `.mylite` file.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Routing Impact

Durable MyLite ordered index reads get scoped storage setup. Volatile
MEMORY/HEAP paths are unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
scope helpers.

## Test Plan

- Re-run storage unit coverage for full index reads, leaf runs, maintained
  roots, and recovery paths.
- Re-run storage-engine smoke because handler ordered index cursors use this
  API.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- `mylite_storage_read_index_entries()` uses scoped file/header helpers.
- Existing full-index root and fallback behavior remains covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- This does not eliminate catalog-image reads for root discovery. That remains
  a separate root metadata cache slice.

## Verification Results

Recorded 2026-05-21:

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  reported no changes.
- `cmake --build --preset dev --target mylite_storage_test` completed with no
  work required.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  libmylite.embedded-storage-engine --output-on-failure` passed.
