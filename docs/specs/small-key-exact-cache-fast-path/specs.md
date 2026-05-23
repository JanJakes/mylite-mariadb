# Small Key Exact Cache Fast Path

## Problem Statement

Prepared primary-key updates still spend samples in exact-index cache lookup,
including `memcmp()` equality checks on small fixed-width keys. The performance
baseline uses `INT NOT NULL PRIMARY KEY`, so its routed point updates repeatedly
probe exact-index caches with small key images.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite first-party storage code owns exact-index cache lookup in
  `packages/mylite-storage/src/storage.c`.
- `find_exact_index_cache_entry_row_id()` already uses the fixed-width
  `key_bytes_equal()` helper for exact cache probes.
- `append_exact_index_cache_matches_to_entryset()` used the helper for its
  first counting pass, but still used `memcmp()` in the second materialization
  pass when copying matching entries into the output entryset.

## Proposed Design

- Add a small-key equality helper for exact-index cache probes.
- Compare 1, 2, 4, and 8 byte key images with fixed-size loads.
- Keep existing `memcmp()` fallback for all other key sizes.
- Leave exact-index hash distribution unchanged until profiling shows a clear
  net win from changing it.

## Implementation Notes

- `key_bytes_equal()` remains the single byte-exact helper for fixed-width
  exact-index cache equality.
- Both passes in `append_exact_index_cache_matches_to_entryset()` now use that
  helper, so exact-entryset materialization no longer falls back to libc
  comparison for 1/2/4/8-byte keys.
- The hash helper and cache bucket layout are unchanged.

## Affected Subsystems

- Exact-index cache lookup and exact-entryset lookup.
- Prepared-update performance baseline.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, hash distribution, or
file-format behavior changes. Equality remains byte-exact.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change.

## Binary Size, License, And Dependencies

Small first-party helper split with no new dependency.

## Test And Verification Plan

- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded smoke tests.
- Run full `storage-smoke-dev` CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run prepared-update performance baseline and sample a long run.

Completed verification:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_statement_test mylite_embedded_storage_engine_test
  mylite_perf_baseline`: pass.
- `git diff --check`: pass.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  pass.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`:
  pass.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine|libmylite.embedded-statement'
  --output-on-failure`: pass, 3/3 tests.
- `ctest --preset storage-smoke-dev --output-on-failure`: pass, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`: prepared
  row-only update step measured 1.660 us/op.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components --profile-iterations=30000000
  10000`: prepared row-only update step measured 1.584 us/op; sample written
  to `/tmp/mylite-small-key-exact-cache-fast-path.sample.txt`. Remaining
  `_platform_memcmp` samples were from SQL row comparison, direct-update shape
  cache checks, and table-definition cache collation lookup, not exact-index
  cache entryset comparison.

## Acceptance Criteria

- Exact-index cache equality for 1/2/4/8 byte keys avoids libc compare
  overhead.
- Existing variable-width and larger key behavior remains unchanged.
- Storage and embedded tests pass.
- Prepared-update profiling reduces exact-index key compare samples or shows
  the next bottleneck clearly.

## Risks And Open Questions

- Extra helper branches could cost more than `memcmp()` on some compilers, so
  perf evidence must stay visible in the slice notes.
