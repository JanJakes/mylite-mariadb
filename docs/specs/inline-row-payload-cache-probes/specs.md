# Inline Row Payload Cache Probes

## Problem

Prepared primary-key updates repeatedly probe active row-payload caches while
reading the current row and replacing the cached payload after mutation. The
probe helpers are small open-addressed hash-table walks, but sampling still
shows `find_row_payload_cache_entry()` and
`find_mutable_row_payload_cache_bucket()` in the prepared-update stack.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `read_indexed_row_payload_from_open_file()` probes the active row-payload
  cache before reading durable row bytes.
- `validate_direct_live_row_in_statement_cache()` treats an active payload-cache
  hit as live-row validation evidence.
- `replace_active_row_payload_in_cache()` probes the mutable bucket before
  updating same-row cached payload bytes.
- Both cache probe helpers contain no allocation, I/O, or rollback side effects.

## Design

- Mark the immutable and mutable row-payload cache probe helpers as hot inline
  functions.
- Keep bucket traversal, tombstone handling, and entry-index validation
  unchanged.
- Leave cache ownership, cache mutation, checksum validation, and eviction
  behavior unchanged.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. The same cache entries are found or missed.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only removes small helper call overhead in active in-memory caches.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- Run prepared-update timing only when unrelated machine load is low.

## Acceptance Criteria

- Row-payload cache lookup semantics remain unchanged.
- Existing active row-payload cache, statement rollback, transaction rollback,
  and embedded storage-engine tests pass.

## Risks

- Forced inlining can increase binary text size slightly. These helpers are
  small, first-party storage probes and are already present in the sampled hot
  path.
