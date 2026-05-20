# Active Payload Validation Cache Threading

## Problem

`validate_direct_live_row_in_statement_cache()` now accepts active row-payload
cache entries as live-row validation proof. In the prepared-update hot path,
`update_row_with_index_entries()` already resolves the active row-payload cache
for post-update maintenance, but validation still walks from the active file
statement to the active cache statement and then scans the payload-cache set
again.

The 2026-05-20 late prepared-update sample still shows this redundant private
cache-owner work under `validate_direct_live_row_in_statement_cache()`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB file changes.
- `update_row_with_index_entries()` resolves `active_cache_statement`,
  `active_live_row_cache`, and `active_row_payload_cache` after table-id
  discovery.
- `validate_direct_live_row_in_statement_cache()` currently rediscovers the
  active cache statement from the file statement solely to locate the payload
  cache.
- Generic validation callers do not always have a pre-resolved payload cache,
  so the existing fallback lookup must remain available.

## Design

- Extend `validate_direct_live_row_in_statement_cache()` with an optional
  pre-resolved active row-payload cache plus a flag that says whether the cache
  lookup was already resolved.
- Pass the pre-resolved cache from `update_row_with_index_entries()`, including
  the `NULL` case, so validation does not repeat the active cache-owner walk on
  every row update.
- Keep `validate_direct_live_row_in_statement()` on the existing generic path
  by passing "not resolved"; it will continue to discover the payload cache when
  needed.
- Preserve active payload validation proof semantics exactly: a retained payload
  entry validates only the same active checkpoint view, and uncached rows still
  fall back to live-row cache and direct row-page validation.

## Affected Subsystems

- First-party MyLite storage active row-payload cache.
- Direct live-row validation inside update/delete style storage paths.
- Prepared primary-key update performance path.

## Compatibility Impact

No SQL, public C API, handler contract, storage-engine routing, file-format, or
durable behavior change. This only removes redundant private lookup work inside
one storage update call.

## Single-File And Lifecycle Impact

No durable file-format, journal, WAL, lock, or companion-file change. The cache
pointer is transient and valid only for the current update call before cache
sets can be cleared or reallocated.

## Binary-Size And Dependency Impact

Tiny private C signature change. No dependency or public symbol impact.

## Tests And Verification

- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 10000 1000000`
- Current verification on 2026-05-20:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`: 10/10 passed.
  - `tools/mylite-perf-baseline --phase=prepared-updates 10000 1000000`:
    repeated prepared primary-key update samples remained in the local noisy
    3.9-4.2 us/op range with checksum 51138894.
  - A late prepared-update sample no longer shows
    `active_cache_statement_from_statement()` under
    `validate_direct_live_row_in_statement_cache()`.

## Acceptance Criteria

- Row-update validation reuses the active row-payload cache pointer already
  resolved by `update_row_with_index_entries()`.
- Generic direct live-row validation still resolves the active payload cache
  itself when the caller does not provide one.
- Existing active payload validation proof regression coverage still passes.
- Prepared-update benchmark completes with the expected checksum and no material
  regression.

## Risks And Unresolved Questions

- A pre-resolved cache pointer must not survive cache-set mutation. This slice
  keeps pointer use inside one update call before the post-update cache
  replacement path mutates the same cache.
- The broader prepared-update path remains dominated by MariaDB planning and
  buffered rewrite/undo work; this is an incremental storage-side cleanup, not a
  full SQLite-like pager solution.
