# Live Row ID Seed Hot Cache

## Problem

The prepared primary-key update sample after live-row validation inlining still
shows `seed_active_live_row_id_cache_in_statement()` and
`find_active_live_row_id_cache()` on the MyLite storage update path. For the hot
prepared update loop, the active live-row-id cache is usually already seeded for
the statement/transaction owner, so the common path only needs to discover that
cache and return.

The durable live-row-id cache lookup and active cache population remain needed
for the first seed and for invalidated metadata, but they should stay out of the
repeated active-cache-hit wrapper.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `update_row_with_index_entries()` in `packages/mylite-storage/src/storage.c`
  calls `seed_active_live_row_id_cache_in_statement()` before validating the
  target row.
- `seed_active_live_row_id_cache_in_statement()` first checks the active
  statement live-row-id cache. On miss it probes the durable live-row-id cache,
  appends an active cache, and copies the durable row-id list into it.
- `find_active_live_row_id_cache()` already keeps a last-hit index for repeated
  table/header lookups.

## Design

- Mark `seed_active_live_row_id_cache_in_statement()` as
  `MYLITE_STORAGE_HOT_INLINE`.
- Mark `find_active_live_row_id_cache()` as `MYLITE_STORAGE_HOT_INLINE`.
- Keep only the active-cache hit/return in the inline seed helper.
- Move durable-cache lookup, active-cache allocation, and row-id list assignment
  to a cold fallback helper used only when the active cache is absent.
- Preserve existing behavior when no durable cache exists or active-cache
  allocation fails.

## Affected Subsystems

- MyLite statement live-row-id cache seeding.
- MyLite durable live-row-id cache lookup.
- Prepared primary-key update validation setup.

## Compatibility Impact

No SQL, public API, handler API, storage-engine routing, metadata, or
file-format behavior change. The same active cache is populated from the same
durable cache on misses.

## Single-File And Lifecycle Impact

No durable storage, journal, lock, or companion-file lifecycle change. The slice
changes only transient cache helper call shape.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party inline/cold split. No dependency or build-profile change.

## Tests And Verification

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample a focused prepared-update benchmark and check whether the live-row-id
  seed wrapper remains a visible storage frame.
- Run `git diff --check` and `git clang-format --diff` on the touched C file.

Completed verification:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure` passed, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` recorded prepared primary-key updates
  at 2.418 us/op.
- The focused sampled run recorded 2.528 us/op in
  `/tmp/mylite-live-row-id-seed-hot-cache.sample`.
  `seed_active_live_row_id_cache_in_statement()` and
  `find_active_live_row_id_cache()` were not visible sampled frames. The cold
  `load_active_live_row_id_cache_in_statement()` fallback remained visible
  during durable-cache promotion. Remaining visible storage work was active
  single-index rewrite, buffered-page undo capture, active row-payload
  replacement, exact-index cache replacement, and handler-side key preparation.
- `git diff --check` passed, and `git clang-format --diff` for
  `packages/mylite-storage/src/storage.c` reported no formatting changes.

## Acceptance Criteria

- Repeated prepared point-update live-row-id seeding returns from the inlined
  active-cache hit path.
- Cache misses still copy durable live row ids into a new active statement cache
  with the existing limits and failure behavior.
- Existing storage and embedded storage-engine tests pass.
- Focused benchmark/profile evidence records the effect and remaining hot path.

## Risks And Open Questions

- This does not remove filename comparison inside the cache match. A future
  owner-scoped file identity cache could avoid that, but it needs a separate
  lifetime design so durable caches do not retain unstable filename pointers.
