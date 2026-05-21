# Inlined Exact Index Key Helpers

## Problem

Prepared primary-key updates repeatedly probe exact-index caches with fixed-width
keys. After the fixed-width hash and compare paths were added, sampling still
shows `hash_fixed_key_value()` and key comparison work in the cache probe path.

These helpers are tiny and called from the hot exact-index lookup loop.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `find_exact_index_cache_entry_row_id()` probes the exact-index cache bucket
  and compares each candidate key with `key_bytes_equal()`.
- `exact_index_cache_bucket_for_key()` and bucket rebuild use
  `hash_key_bytes()`, which delegates common 1/2/4/8-byte keys to
  `hash_fixed_key_value()`.
- `MYLITE_STORAGE_HOT_INLINE` is already used for small storage helpers in
  page rewrite and fixed-width field access paths.

## Design

- Mark `key_bytes_equal()`, `hash_key_bytes()`, and `hash_fixed_key_value()` as
  `MYLITE_STORAGE_HOT_INLINE`.
- Keep the exact hash values and equality semantics unchanged.
- Do not inline larger cache-walking functions, preserving the current code-size
  profile outside these tiny helpers.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. Exact-index cache lookup returns the same rows with the same
hash bucket mapping.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- Run prepared-update timing only when unrelated machine load is low.

## Acceptance Criteria

- Fixed-width exact-index cache hash and compare helpers are inlined at their
  hot call sites.
- Existing exact-index, row-DML, transaction, rollback, and embedded
  storage-engine tests pass.

## Risks

- Forced inlining can increase code size. This slice limits inlining to three
  tiny helpers already dedicated to exact-index cache probe work.
