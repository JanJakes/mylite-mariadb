# Active Row-Payload Last-Bucket Hint

## Problem

Prepared exact-key row-only updates read the target row through the active
row-payload cache and then immediately validate the same row id before mutating
the active checkpoint. Both operations probe the same open-addressed
row-payload bucket table. The work is small, but it is on the hot
`prepared-row-only-update-components` step path after larger storage lookups
have already been removed.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is limited to first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source change is
  required.
- `read_indexed_row_payload_from_open_file()` probes the active
  row-payload cache before copying the indexed row into MariaDB's record
  buffer.
- `update_row_with_index_entries_for_context()` then calls
  `validate_direct_live_row_in_statement_cache()`, which probes the same
  active row-payload cache by row id to prove the row is valid in the active
  checkpoint.
- `replace_active_row_payload_in_cache()` already accepts a known bucket from
  validation, so the remaining redundant bucket probe is between target-row
  read and validation.
- Row-payload buckets move only when the bucket array is rebuilt. Insertions,
  deletions, removals, and rebuilds therefore need to maintain or clear any
  cached bucket hint.

## Design

Add a one-entry last-bucket hint to `mylite_storage_row_payload_cache`:

- remember the bucket index for successful row-id lookups,
- check the remembered bucket before hashing/probing the bucket table again,
- clear the hint before mutations that can invalidate bucket state,
- refresh the hint when appending or inserting a standalone row-payload entry,
- clear the hint after deleting a bucket or rebuilding the bucket array.

Use the remembering lookup for active indexed-row cache reads, durable cache
reads, builder materialization, active update validation, and active cache
replacement fallback. The hint never changes cache ownership, row visibility,
rollback behavior, or durable invalidation; stale hints are validated against
the bucket state, row id, and entry index before use.

## Compatibility Impact

No SQL-visible, storage-engine routing, public API, or file-format behavior
changes. The cache still returns the same row bytes, and all misses fall back to
the existing hash probe and storage read paths.

## Single-File And Embedded Lifecycle Impact

No durable `.mylite` layout, journal, lock, recovery, or companion-file
lifecycle change. The hint is transient statement or thread-local cache state.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change. A storage test hook
is added under `MYLITE_STORAGE_TEST_HOOKS` only.

## Binary-Size And Dependency Impact

No dependency change. Binary-size impact is limited to a few fields and hot
inline helper branches in the first-party storage library.

## Tests And Verification Plan

- Added storage unit coverage proving an active indexed-row read seeds the
  row-payload cache hint before the following update validates and replaces the
  cached row.
- Passed `git diff --check`.
- Passed `git clang-format --diff -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`.
- Passed `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Passed `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Passed `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`:
  - bind: `0.022 us/op`
  - step: `1.321 us/op`
  - reset: `0.022 us/op`
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components --profile-iterations=10000000
  10000`:
  - bind: `0.022 us/op`
  - step: `1.296 us/op`
  - reset: `0.022 us/op`
- Ran a sampled 10M-iteration `prepared-row-only-update-components` benchmark:
  - bind: `0.022 us/op`
  - step: `1.304 us/op`
  - reset: `0.022 us/op`
  - sample written to
    `/tmp/mylite-active-row-payload-last-bucket-hint.sample.txt`.

## Acceptance Criteria

- Active row-payload cache reads remember the bucket used for the last row-id
  hit.
- Immediate update validation can reuse that bucket when it is still valid.
- Cache mutation paths invalidate or safely refresh the hint so stale bucket
  indexes cannot return wrong rows.
- Existing storage and embedded storage-engine tests pass.
- Prepared row-only update timing does not regress.

## Risks And Unresolved Questions

- This is a narrow cache-control optimization. If the current profile is
  dominated by row bytes copied into MariaDB record buffers or by SQL-layer
  expression evaluation, timing gains may be small.
- The hint must stay local to a single row-payload cache object. It must not be
  shared across active statement caches or durable cache generations.
