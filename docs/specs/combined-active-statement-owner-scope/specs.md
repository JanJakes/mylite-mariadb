# Combined Active Statement Owner Scope

## Problem

The row-update hot path opens the active update file scope, then immediately
walks the active statement parent chain twice: once to find the append-buffer
owner for active buffered rewrites, and once to find the outer active cache
owner for table, row-payload, live-row, and exact-index caches.

The sampled prepared-update profile still shows
`append_page_buffer_statement_from_statement()` and
`active_cache_statement_from_statement()` after earlier filename-based owner
lookups were removed.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `open_existing_file_for_update_scope()` records the active statement that
  owns the already-open file.
- `append_page_buffer_statement_from_statement()` walks the parent chain and
  returns the outermost same-file same-owner statement.
- `active_cache_statement_from_statement()` walks the same parent chain and
  returns the outermost same-owner statement.
- `update_row_with_index_entries()` calls both helpers back-to-back for every
  row update.

## Design

- Add one helper that resolves both the outer active cache owner and the
  outer same-file append-buffer owner in a single parent-chain pass.
- Use the combined helper only in `update_row_with_index_entries()`, where both
  owners are needed immediately.
- Keep the existing single-purpose helpers for callers that need only one owner
  or start from a `FILE *`.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. The same owners are selected with fewer parent-chain walks.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only passes existing transient active-statement ownership through
the row-update implementation.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- Run the prepared-update benchmark when unrelated machine load is low enough
  for meaningful timing.

## Acceptance Criteria

- `update_row_with_index_entries()` resolves active cache and append-buffer
  owners with one parent-chain pass.
- Existing active buffered rewrite, savepoint rollback, transaction rollback,
  and embedded storage-engine tests pass.

## Risks

- The helper must preserve the distinction between outermost same-owner cache
  state and outermost same-file append-buffer state. It keeps both conditions
  explicit in the single pass.
