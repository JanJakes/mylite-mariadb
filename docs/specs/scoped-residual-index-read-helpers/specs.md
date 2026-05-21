# Scoped Residual Index Read Helpers

## Problem

Most durable index reads now use `mylite_storage_file_scope`, but two residual
index helpers still have older lifecycle behavior:

- `mylite_storage_index_prefix_exists()` still uses generic
  open/header/close before scanning all index-entry and maintained-root pages.
- `mylite_storage_find_index_entry()` already opens a file scope, but releases
  it through the raw file closer instead of `close_existing_file_scope()`.

These paths should match the scoped ownership pattern used by exact-entryset,
indexed-row, and index-specific prefix reads.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- `mylite_storage_read_index_entries()`,
  `mylite_storage_read_exact_index_entries()`,
  `mylite_storage_index_prefix_exists_for_index()`, and
  `find_indexed_row_payload()` already use scoped file/header reads.
- `mylite_storage_index_prefix_exists()` is the remaining broad prefix scan
  that resolves only a table id and then scans index-entry/root pages.
- `mylite_storage_find_index_entry()` uses scoped setup but closes with the
  legacy helper.

## Scope

- Move `mylite_storage_index_prefix_exists()` to `open_existing_file_scope()`.
- Use `read_header_from_file_scope()`.
- Resolve the table id through `find_table_id_in_statement()` when an active
  cache statement is available.
- Close both affected helpers through `close_existing_file_scope()`.
- Remove the now-unused `open_existing_file()` wrapper after the last read
  caller moves to scoped setup.
- Preserve current page scanning, row-state filtering, match maintenance, and
  error behavior.

## Non-Goals

- No SQL behavior change.
- No public C API change.
- No file-format change.
- No change to index-specific FK prefix probes.
- No B-tree navigation, page split, or multi-page maintained-root mutation.

## Design

The broad prefix helper opens a `mylite_storage_file_scope`, reads the scoped
header, resolves the table id via the active table-entry cache when available,
and then runs the existing page scan unchanged. Exact point lookup keeps its
current setup and lookup logic, but releases the scope through the matching
scope closer. Once all read callers use explicit file scopes, the legacy
`open_existing_file()` wrapper is dead code and is removed.

## Compatibility Impact

SQL-visible behavior is unchanged. The same row-state and prefix comparison
rules apply; only file/header ownership is aligned with the rest of the index
read stack.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. Borrowed active,
read-statement, read-snapshot, and transaction-journal snapshot files are
released through the same scoped helper used by newer index read paths.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite broad prefix scans benefit. Runtime-volatile MEMORY/HEAP handler
paths are unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
scope and cache helpers.

## Test Plan

- Re-run storage unit coverage for exact point lookup, prefix-exists scans,
  maintained-root visibility, row-state filtering, locking, rollback, and
  recovery.
- Re-run storage-engine smoke because index reads and FK checks share the same
  lifecycle helpers.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- `mylite_storage_index_prefix_exists()` uses scoped file/header setup.
- `mylite_storage_find_index_entry()` closes through
  `close_existing_file_scope()`.
- The unused generic read-open wrapper is removed.
- Existing index lookup and prefix behavior remains covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- The broad prefix helper remains a scan-based fallback. Full SQLite-like point
  performance still depends on the planned navigable index and pager work.

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
