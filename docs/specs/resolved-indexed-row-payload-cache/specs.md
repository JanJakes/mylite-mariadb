# Resolved Indexed Row Payload Cache

## Problem

Prepared primary-key update execution reads the target row through
`find_indexed_row_payload()`, resolves the active cache statement once, then
`read_indexed_row_payload_from_open_file()` re-finds the active row-payload
cache by filename, catalog generation, and table id before probing the cached
row payload.

The sampled prepared-update profile still shows
`active_row_payload_cache_entry_for_statement()`,
`active_row_payload_cache_for_statement()`, and
`find_active_row_payload_cache()` in the indexed-row read path after earlier
active statement lookups were removed.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `find_indexed_row_payload()` resolves the active cache statement from the
  opened file scope before reading the target row.
- `active_row_payload_cache_for_resolved_statement()` returns the matching
  active row-payload cache using the already known header and table id.
- `read_indexed_row_payload_from_open_file()` only needs the active cache for a
  best-effort active cache hit before falling back to durable cache and page
  reads.

## Design

- Resolve the active row-payload cache in `find_indexed_row_payload()` after
  the table id is known.
- Pass that cache pointer into `read_indexed_row_payload_from_open_file()` for
  the active cache probe.
- Keep the existing miss path unchanged: durable cache validation, page read,
  active cache store, durable cache store, and validated-live marking retain
  the same behavior.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. The same active row-payload cache entry is consulted with one
less lookup chain.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only passes an existing transient cache pointer within one open file
scope.

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

- Indexed-row payload reads use the caller-resolved active row-payload cache for
  the active cache hit path.
- Existing routed storage, statement rollback, transaction rollback, and
  embedded storage-engine tests pass.

## Risks

- The pointer is only valid while the active statement cache set remains
  unchanged. This slice passes it across the immediate indexed-row payload read
  only; cache creation and clearing still happen in the existing miss path.
