# Scoped Exact Entryset Read

## Problem

Secondary exact-index reads that return multiple row ids use
`mylite_storage_read_exact_index_entries()`. That helper still opens the file
through `open_existing_file()` and reads the header through the generic
`read_header()` path, while point exact lookups and index-specific prefix probes
already use `mylite_storage_file_scope`.

The performance baseline still shows secondary exact selects as a hot path.
Before deeper navigable indexes, exact entryset reads should at least reuse the
same scoped file/header and active table-entry cache path as adjacent exact
lookup helpers.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::mylite_storage_find_index_entry()`
  uses `open_existing_file_scope()`, `read_header_from_file_scope()`, and the
  active table-entry cache before exact lookups.
- `packages/mylite-storage/src/storage.c::mylite_storage_index_prefix_exists_for_index()`
  now uses the same scoped read setup.
- `packages/mylite-storage/src/storage.c::mylite_storage_read_exact_index_entries()`
  still uses `open_existing_file()`, `read_header()`, and always performs table
  lookup through a copied catalog image.

## Scope

- Move `mylite_storage_read_exact_index_entries()` onto
  `mylite_storage_file_scope`.
- Use `read_header_from_file_scope()` so active statements, read statements,
  snapshots, and transaction-journal snapshots reuse their current header view.
- Reuse the active table-entry cache when present.
- Keep catalog-backed index-root discovery and exact-entry fallback behavior
  unchanged.

## Non-Goals

- No SQL behavior change.
- No file-format change.
- No new public API.
- No maintained B-tree split/merge work.
- No attempt to avoid the catalog image for index-root lookup; that belongs to
  a later root-cache slice.

## Design

Mirror the existing exact point lookup setup:

1. Open `mylite_storage_file_scope`.
2. Read the header with `read_header_from_file_scope()`.
3. Check the active table-entry cache and populate it on miss.
4. Ensure a catalog image exists because static root discovery still needs it.
5. Close through `close_existing_file_scope()`.

The exact-entry implementation then keeps the same leaf-root path, durable
exact-index cache path, and append-history scan fallback.

## Compatibility Impact

No SQL-visible compatibility change. Exact secondary index reads return the
same entryset and preserve existing row-visibility behavior.

## Single-File And Lifecycle Impact

No lifecycle change. The helper continues to read one `.mylite` file and
introduces no companion files.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Routing Impact

Durable MyLite secondary exact reads use scoped storage reads. Volatile
MEMORY/HEAP paths are unchanged.

## Binary-Size, License, And Dependency Impact

No dependency or license change. Binary impact is limited to using existing
scope helpers.

## Test Plan

- Re-run storage unit coverage for exact entryset, leaf-run, maintained-root,
  rollback, and cache paths.
- Re-run the storage-engine smoke because handler secondary exact reads use the
  exact-entryset API.
- Run `git diff --check` and `git clang-format --diff` for touched C files.

## Acceptance Criteria

- `mylite_storage_read_exact_index_entries()` uses scoped file/header helpers.
- Existing exact-entryset static-root and fallback behavior remains covered.
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

- This does not remove the remaining catalog-image read/copy when index-root
  discovery is needed. A root metadata cache is still a separate roadmap slice.
