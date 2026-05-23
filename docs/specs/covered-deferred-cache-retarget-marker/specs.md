# Covered Deferred Cache Retarget Marker

## Problem Statement

Prepared row-only updates inside one transaction now defer durable-cache
retargeting to statement commit, but each nested row-DML statement still records
and merges its own deferred retarget marker. Once a row has an active
append-buffer replacement, later row-only updates can rewrite that buffered row
in place without advancing the durable header. If the statement chain already
has a marker for that table, those active in-place rewrites add no new
durable-cache invalidation information.

The local prepared row-only update profile still shows
`defer_durable_cache_retarget_after_table_mutation()` and
`merge_deferred_durable_cache_retarget()` on the hot path after row-only active
rewrites.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source change is
  required.
- `defer_durable_cache_retarget_after_table_mutation()` records the mutated
  table id, all-tables fallback flag, and compact durable header fingerprint on
  the active `mylite_storage_statement`.
- `merge_deferred_durable_cache_retarget()` merges child markers into a parent
  statement on nested commit.
- Nested rollback discards the child statement and its deferred marker.
- `apply_deferred_durable_cache_retarget()` applies a parent marker before
  active cache promotion at top-level commit.
- `rewrite_active_update_pages()` reports when an update was handled by an
  active in-place rewrite. For row-only rewrites, the row id and active header
  remain unchanged.

## Proposed Design

- After a successful row mutation, skip
  `retarget_durable_caches_after_table_mutation_in_statement()` only when:
  - the mutation used the active in-place rewrite path, and
  - the statement chain already has a same-table or all-tables deferred durable
    retarget marker.
- Keep the existing marker update path for append-style row replacements,
  deletes, inserts, first active rewrites without a covering marker, and
  mutations of another table that must escalate to the all-tables fallback.
- Keep the small retarget marker helpers hot-inline so the remaining first
  marker path does not leave extra function-call frames in row-DML profiles.

## Implementation Notes

- `update_row_with_index_entries_for_context()` skips the deferred durable
  retarget call after an active in-place rewrite when
  `statement_chain_has_deferred_durable_cache_retarget()` already finds a
  covering marker for the mutated table.
- Append-style replacements, inserts, deletes, and active rewrites without a
  covering marker still record deferred durable-cache work.
- The compact marker write helpers are hot-inlined; a broader header-fingerprint
  coverage check was rejected during implementation because sampling showed it
  added comparison cost to every row-only update.

## Affected Subsystems

- MyLite storage statement lifecycle.
- Deferred durable-cache invalidation and retarget markers.
- Prepared row-DML storage performance.

## Compatibility Impact

No SQL, C API, storage-engine routing, DDL metadata, or file-format behavior
changes. The optimization only skips a child marker when an existing retained
marker would produce the same durable-cache invalidation at top-level commit.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, sidecar, recovery, or handle-lifecycle change.
The skipped marker is transient statement bookkeeping.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Binary-Size, License, And Dependency Impact

Small first-party helper code only. No dependency or license change.

## Test And Verification Plan

- Add storage regression coverage proving that repeated nested commits under a
  parent transaction keep one parent deferred marker while still clearing stale
  durable row-payload caches at top-level commit.
- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded tests.
- Run the full `storage-smoke-dev` CTest suite.
- Run `git diff --check` and `git clang-format --diff`.
- Run the prepared row-only update component baseline and a focused sample.

Completed verification:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_statement_test mylite_embedded_storage_engine_test
  mylite_perf_baseline`: pass.
- `git diff --check`: pass.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c`: pass.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`:
  pass.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine|libmylite.embedded-statement'
  --output-on-failure`: pass, 3/3 tests.
- `ctest --preset storage-smoke-dev --output-on-failure`: pass, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`: prepared
  row-only update step measured 1.677 us/op.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components --profile-iterations=5000000
  10000`: prepared row-only update step measured 1.545 us/op; sample written
  to `/tmp/mylite-covered-deferred-cache-retarget-marker-v2.sample.txt`.
  Repeated `defer_durable_cache_retarget_after_table_mutation()` and
  `retarget_durable_caches_after_table_mutation_in_statement()` frames were not
  present; the remaining marker-related frames were small
  `statement_chain_has_deferred_durable_cache_retarget()` and nested-commit
  merge samples.

## Acceptance Criteria

- A child statement skips its durable-cache retarget marker only for active
  in-place rewrites already covered by a same-table or all-tables marker in the
  statement chain.
- Header-advancing mutations still record or refresh the retained marker to the
  latest fingerprint.
- Different-table mutations still escalate to the all-tables fallback.
- Nested rollback remains safe because the skipped marker is only skipped when
  an ancestor marker already covers the committed final invalidation.
- Existing storage, embedded statement, and embedded storage-engine tests pass.
- Prepared row-only update profiling no longer shows repeated covered child
  retarget markers as a hot path.

## Risks And Open Questions

- The skip relies on active in-place rewrites not advancing the header. If a
  future active rewrite path mutates header identity, it must keep the existing
  retarget marker path.
