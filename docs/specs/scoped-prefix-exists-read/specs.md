# Scoped Prefix Exists Read

## Problem

`mylite_storage_index_prefix_exists_for_index()` still opens the durable file
through the generic `open_existing_file()` / `read_header()` path. Exact index
lookups already use `mylite_storage_file_scope` so active statements, read
statements, and snapshots can reuse their current file/header view.

Foreign-key row checks call the prefix-exists helper from hot row-DML paths, so
it should follow the same scoped read pattern as exact point lookups.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::mylite_index_prefix_exists()` calls
  `mylite_storage_index_prefix_exists_for_index()` for durable FK probes.
- `packages/mylite-storage/src/storage.c::mylite_storage_find_index_entry()`
  and `find_indexed_row_payload()` use `open_existing_file_scope()`,
  `read_header_from_file_scope()`, and the active table-entry cache before
  exact index reads.
- `packages/mylite-storage/src/storage.c::mylite_storage_index_prefix_exists_for_index()`
  still uses the older generic open/header path.

## Scope

- Move index-specific prefix-exists reads onto `mylite_storage_file_scope`.
- Use `read_header_from_file_scope()` for active statement/read/snapshot header
  reuse.
- Reuse the active table-entry cache when present, while still reading the
  catalog image because static root discovery needs catalog root records.
- Keep static-root fast path and materialized fallback behavior unchanged.

## Non-Goals

- No SQL behavior change.
- No file-format change.
- No new public API.
- No change to FK action semantics or unsupported FK shapes.

## Design

Mirror the exact-index read setup:

1. open a `mylite_storage_file_scope`;
2. read the current header from that scope;
3. try the active table-entry cache;
4. read the catalog image exactly once for root discovery and fallback;
5. close through `close_existing_file_scope()`.

The fallback still materializes entries when a static root is missing or has an
append tail. The static path still reads complete leaf/maintained roots through
the existing root helper.

## Compatibility Impact

No SQL-visible behavior change. FK prefix checks keep the same answer and error
mapping.

## Single-File And Lifecycle Impact

No lifecycle change. Scoped reads preserve the same single-file ownership rules
while avoiding redundant generic header reads when an active scope already owns
the file.

## Public API And File-Format Impact

No API or file-format change.

## Storage-Routing Impact

Durable MyLite FK prefix probes use scoped storage reads. Volatile MEMORY/HEAP
FK paths are unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to reusing existing
scope helpers.

## Test Plan

- Re-run storage unit coverage, including static and maintained prefix-exists
  tests.
- Re-run the storage-engine smoke because the handler FK path calls this API.
- Run `git diff --check` and `git clang-format --diff` for touched files.

## Acceptance Criteria

- `mylite_storage_index_prefix_exists_for_index()` uses scoped file/header
  helpers.
- Existing static-root and fallback prefix behavior remains covered.
- Storage and storage-smoke tests pass.

## Verification Results

2026-05-21, macOS arm64 local worktree:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure
```

All passed.

## Risks And Open Questions

- The helper still reads the catalog image even when the table entry cache hits,
  because root lookup is catalog-backed. Avoiding that needs a separate
  catalog-root/index-root cache slice.
