# Scoped Row Read APIs

## Problem

Primary-key and index-oriented reads now use `mylite_storage_file_scope` and
scoped header reads when an active statement, read statement, or snapshot owns a
stable file view. Several row-oriented public storage APIs still use
`open_existing_file()` plus generic `read_header()`, even though they are common
full-scan and materialization paths:

- `mylite_storage_read_rows()`
- `mylite_storage_count_rows()`
- `mylite_storage_read_indexed_rows()`
- `read_row_payload()` for `mylite_storage_read_row()` and
  `mylite_storage_read_indexed_row()`

These helpers should reuse the same scoped file/header setup as the newer
point and index-entryset reads.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source change is required. The affected code is first-party
  MyLite storage in `packages/mylite-storage/src/storage.c`.
- `mylite_storage_find_index_entry()`,
  `find_indexed_row_payload()`, exact entryset reads, prefix probes, and full
  entryset reads already use `mylite_storage_file_scope` and
  `read_header_from_file_scope()`.
- The row-read APIs above still call `open_existing_file()`, `read_header()`,
  and `close_existing_file()` directly.
- `find_table_id_in_statement()` can reuse the active table-entry cache when a
  same-owner active checkpoint is available.

## Scope

- Move the row-read APIs listed above to `open_existing_file_scope()`.
- Use `read_header_from_file_scope()`.
- Resolve table ids through `find_table_id_in_statement()` with the active
  cache statement derived from the scoped active statement.
- Close through `close_existing_file_scope()`.
- Preserve current row visibility, durable live-row cache, row-payload cache,
  and exact error behavior.

## Non-Goals

- No SQL behavior change.
- No file-format change.
- No public API change.
- No rowset cache redesign.
- No B-tree, pager, WAL, or write-concurrency work.

## Design

Each target helper opens a `mylite_storage_file_scope`, derives the borrowed or
owned `FILE *`, reads the scoped header, and resolves the table id through the
active table-entry cache when available.

The rest of each helper remains unchanged: full row reads still collect live row
ids, counts remain exact, direct row reads still validate visibility when
requested, and indexed-row batch materialization still consults active and
durable row-payload caches.

## Compatibility Impact

SQL-visible behavior is unchanged. The scoped path reads the same checkpoint
view already selected by active/read statement ownership and keeps the existing
row visibility checks.

## Single-File And Lifecycle Impact

No durable lifecycle change and no new companion files. The slice narrows
transient file-handle ownership so borrowed statement files are closed through
the scope helper.

## Public API And File-Format Impact

No public C API or file-format change.

## Storage-Routing Impact

Durable MyLite full scans, exact row reads, counts, and indexed-row
materialization benefit. Volatile MEMORY/HEAP handler storage paths are
unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
scope helpers.

## Test Plan

- Re-run storage unit coverage for full-row scans, counts, direct row reads,
  indexed-row materialization, row caches, read statements, locking, rollback,
  and recovery.
- Re-run storage-engine smoke because handler scans and row materialization use
  these APIs.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- The listed row-read APIs use scoped file/header setup.
- Existing row visibility, cache, and error behavior remains covered.
- Storage and storage-smoke tests pass.

## Risks And Open Questions

- This still leaves many metadata and write helpers on older open/header
  wrappers. Those should be migrated in smaller follow-up slices where the
  ownership and locking behavior is clear.

## Verification Results

Recorded 2026-05-21:

- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  reported no changes.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  libmylite.embedded-storage-engine --output-on-failure` passed.
