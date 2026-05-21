# Thread Active Payload Cache Entry

## Problem

Prepared point updates read the current row through the active row-payload cache
and then update that same cache entry after the storage mutation. The update
path already resolves the active cache once, but
`replace_active_row_payload_in_cache()` still performs another bucket lookup for
the same row id.

Sampling shows `replace_active_row_payload_in_cache()` on the remaining
prepared-update storage stack.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `validate_direct_live_row_in_statement_cache()` treats an active
  row-payload-cache hit as live-row validation evidence.
- `update_row_with_index_entries()` calls that validation helper before
  rewriting row/index pages and later calls
  `replace_active_row_payload_in_cache()` for the same active payload cache.
- Same-row active rewrites preserve the row id, so the validated cache entry is
  still the entry that needs an in-place payload refresh.

## Design

- Let `validate_direct_live_row_in_statement_cache()` optionally return the
  active row-payload cache entry used for validation.
- Thread that entry through `update_row_with_index_entries()`.
- Teach `replace_active_row_payload_in_cache()` to use a trusted known entry
  when it still represents the old row id, while preserving the existing bucket
  lookup fallback.
- Keep row-size limit checks, allocation behavior, checksum invalidation,
  row-id-changing fallback, and cache eviction behavior unchanged.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. Active cache replacement updates the same cached payload entry
or falls back to the same lookup/removal behavior.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only reuses transient active cache identity within one storage update
call.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- Run prepared-update timing only when unrelated machine load is low.

## Acceptance Criteria

- Same-row active row-payload cache replacement can reuse the validation entry
  without another bucket lookup.
- Existing active row-payload cache, statement rollback, transaction rollback,
  and embedded storage-engine tests pass.

## Risks

- The threaded entry must not be used after cache mutation or reallocation. The
  update path does not mutate the active row-payload cache between validation
  and replacement, and the replacement helper still falls back when the known
  entry does not represent the old row id.
