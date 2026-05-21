# Inline Exact Index Cache Probes

## Problem

Prepared primary-key updates repeatedly probe active exact-index caches while
building the handler cursor for the row selected by primary key. The fixed-width
key hash and compare helpers are already hot-inlined, but sampling still shows
the small exact-index cache probe wrappers in the prepared-update stack.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `find_cached_exact_index_entry_in_statement()` locates a table/index/key-size
  cache with `find_exact_index_cache()`.
- Once the cache exists, `find_exact_index_cache_entry_row_id()` ensures the
  bucket index is available and walks a single hash bucket for the matching key.
- The hot prepared-update path reuses already-created active exact-index caches;
  cache creation, seeding, and durable fallback remain outside this slice.

## Design

- Mark `find_exact_index_cache()` as a hot inline cache-set probe.
- Mark `find_exact_index_cache_entry_row_id()` as a hot inline exact-key row-id
  probe.
- Keep bucket construction, bucket traversal, live-entry filtering, key compare,
  and row-id output semantics unchanged.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. Exact-index cache hits and misses produce the same row ids.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only removes helper call overhead in active in-memory exact-index
caches.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- Run prepared-update timing only when unrelated machine load is low.

## Acceptance Criteria

- Active exact-index cache probe semantics remain unchanged.
- Existing exact-index lookup, statement rollback, transaction rollback, and
  embedded storage-engine tests pass.

## Risks

- Forced inlining can increase binary text size slightly. The inlined helpers
  are small and already sit on the sampled prepared-update path.
