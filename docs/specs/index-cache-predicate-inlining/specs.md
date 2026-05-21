# Index Cache Predicate Inlining

## Problem

After active cache lookup short-circuits, prepared primary-key update samples
still show tiny first-party predicates on the hot path, including
`is_index_entry_changed()` and `exact_index_cache_bucket_for_key()`. These
helpers are simple branch/hash wrappers called inside row update, active
rewrite, and exact-index cache loops.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `update_row_with_index_entries()` and its row/index publication helpers test
  per-index change masks repeatedly while maintaining append-only index pages
  and active exact-index caches.
- Exact-index cache probes already use hash buckets and fixed-width key hashing,
  but the bucket wrapper itself still appears in prepared-update samples.

## Design

- Mark `changed_index_entry_count()` and `is_index_entry_changed()` as hot
  inline helpers.
- Mark `exact_index_cache_bucket_for_key()` as a hot inline helper so exact
  cache probes can inline the fixed-width hash path directly.
- Keep key hashing, changed-entry semantics, exact-index cache ordering, and
  active cache invalidation unchanged.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. The same index-entry change masks and exact-index cache keys
are evaluated with the same logic.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

## Acceptance Criteria

- Hot index-entry mask checks and exact-cache bucket resolution compile as
  inline storage helpers.
- Existing storage and embedded routed-storage tests pass.

## Risks

- This is a micro-optimization and should stay limited to small predicates so it
  does not obscure row/index maintenance logic or materially grow the embedded
  profile.
