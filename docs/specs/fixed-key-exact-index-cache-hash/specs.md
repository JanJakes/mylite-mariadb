# Fixed-Key Exact-Index Cache Hash

## Problem

Prepared primary-key updates repeatedly probe the active exact-index cache for
the same fixed-width primary-key shape. Existing exact-index cache equality
already has specialized 1/2/4/8-byte comparisons, but bucket selection still
hashes every byte with the generic FNV-1a loop. In the current sampled
prepared-update profile, `hash_key_bytes()` remains visible under
`find_exact_index_cache_entry_row_id()` for 8-byte point keys.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `packages/mylite-storage/src/storage.c::find_exact_index_cache_entry_row_id()`
  computes a bucket from `exact_index_cache_bucket_for_key()` before comparing
  candidate keys.
- `key_bytes_equal()` already specializes 1/2/4/8-byte key equality with
  unaligned-safe `memcpy()` loads.
- `hash_key_bytes()` still uses the generic byte loop for every key size,
  including the fixed-width integer keys used by the prepared-update baseline.
- Exact-index cache buckets are transient process memory. The bucket hash is
  not persisted, exposed through the public API, or part of the file format.

## Design

- Keep `hash_key_bytes()` as the exact-index cache bucket hash entry point.
- For 1/2/4/8-byte keys, load the fixed-width key image with `memcpy()` and use
  a small integer mix instead of the per-byte FNV loop.
- Keep the generic FNV-1a byte loop for variable-width and larger keys.
- Mix the key size into fixed-width hashes so equivalent numeric values from
  differently sized caches do not collapse to the same hash pattern.

## Compatibility Impact

No SQL, public C API, storage-engine routing, or file-format behavior changes.
Only transient exact-index cache bucket placement changes.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates 1000 1000000`
  - sampled prepared-update run to confirm the fixed-key hash loop is no longer
    the dominant exact-index cache bucket cost
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

## Acceptance Criteria

- Exact-index cache bucket lookup uses fixed-width hashing for 1/2/4/8-byte
  keys.
- Variable-width key hashes keep the existing byte-wise behavior.
- Focused storage and embedded storage-engine tests pass.
- Prepared-update performance is neutral or improved.

## Risks

- Poor bucket distribution would increase exact-index cache collisions. The
  fixed-width path uses the same multiply/xor style already used for row-id
  bucket hashes, with the key size included in the value before mixing.
