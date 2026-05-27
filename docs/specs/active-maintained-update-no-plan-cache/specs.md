# Active Maintained Update No-Plan Cache

## Problem

The prepared update path counters show that changed-index prepared updates run
maintained-root update planning on every iteration even when the plan is empty.
On the 1000-row / 100000-iteration `prepared-update-components` phase, storage
reported `100000` maintained-root plan checks, `0` maintained-root update
writes, `0` retarget writes, `1000` inline update writes, and then `99000`
active buffered rewrite successes.

After the first non-maintained replacement for a row, the new row id is an
append-history or active-buffer row. It cannot be physically present in a
catalog-published maintained root until a later maintained-root writer puts it
there. Rechecking the maintained-root planner for the same active replacement
row id in later prepared executions is redundant.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` routes
  changed-index direct updates to
  `mylite_storage_update_row_with_index_entry_changes_in_statement()` when a
  MyLite storage statement is active.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries_for_context()`
  reads catalog root metadata and calls
  `plan_maintained_index_root_updates()` before attempting active buffered
  rewrite.
- `plan_maintained_index_root_updates()` only suppresses fallback index-entry
  publication when it can physically update a maintained root or branch page.
  Empty plans preserve correctness by leaving the normal append-tail overlay.
- Active buffered rewrite keeps the row id unchanged. If a prior non-maintained
  update produced the active replacement row, later same-row active rewrites do
  not need maintained-root planning until catalog identity changes or rollback
  invalidates active caches.

## Design

Add a transient active-statement no-plan cache keyed by
`(catalog_root_page, catalog_generation, table_id, row_id)`.

- Consult the cache only for changed-index updates with index entries.
- Do not consult it for preserving-index updates, because those may need
  maintained-root retarget planning when the source row still lives in a
  single-page root.
- Populate it after a successful update that had no maintained-root update plan
  and no maintained-root retarget plan. The stored row id is the resulting row
  id, not the old source row id.
- Clear it with catalog-root cache invalidation and nested rollback cache
  invalidation.
- Treat allocation failure as a missed optimization, not a storage error.

The cache does not claim that a table has no maintained roots. It only records
that a specific active replacement row id has already reached the append-tail
overlay path under a specific catalog identity.

## Affected Subsystems

- MyLite storage row-update planning.
- Active statement transient caches.
- Storage-smoke prepared update performance evidence.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, storage-engine routing,
metadata, or diagnostics change is intended. Rows still publish replacement
row-state and index-entry overlay pages exactly as before when no maintained
root update applies.

## Single-File And Embedded Lifecycle Impact

No durable `.mylite` file-format, journal, lock, recovery, sidecar, or embedded
lifecycle change. The cache is in-memory active statement state and is cleared
on rollback or catalog cache invalidation.

## Public API And File-Format Impact

No public API and no file-format change. Added accessors are private storage
test-hook symbols for verification and benchmark output.

## Binary-Size And Dependency Impact

Small first-party C cache state using existing row-id set helpers. No new
dependency.

## Tests And Verification Plan

- Add storage coverage that creates catalog-published index-root metadata after
  an append-history row, performs one non-maintained changed-index update, then
  verifies the second active update skips maintained-root planning through the
  no-plan cache.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 100000`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-row-only-update-components 1000 100000`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Verification Results

Verified on the VPS handoff environment on 2026-05-27:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in 301.82 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed
  in 306.24 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 100000`: passed; the step component measured
  207.693 us/op, maintained-root update plans dropped to `1000`, no-plan
  cache hits were `99000`, stores were `1000`, active rewrite successes were
  `99000`, inline update writes were `1000`, and append update writes were `0`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-row-only-update-components 1000 100000`: passed; the row-only step component measured
  241.056 us/op, maintained-root update plans remained `0`, no-plan cache hits
  were `0`, no-plan cache stores were `0`, active row-only rewrite successes
  were `99000`, and append update writes were `0`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed; the insert step component measured
  291.684 us/op and the existing insert storage counters stayed within the
  current expected shape for this VPS run.

## Acceptance Criteria

- Changed-index active updates skip maintained-root planning only after a
  row-specific no-plan cache hit.
- Preserving-index row-only updates do not use the no-plan cache.
- Nested rollback clears parent no-plan caches along with other active row/index
  caches.
- Prepared update counters show maintained-root plan checks drop on the
  changed-index benchmark while row-only counters remain unchanged.
- Existing storage and embedded storage-engine tests pass.

## Risks And Unresolved Questions

- The cache relies on append-only row id ownership: once a successful
  non-maintained update returns a replacement row id, that row id is not in a
  maintained root unless a later maintained-root writer explicitly handles it.
- Future physical branch-update maintenance that rewrites broader row sets must
  keep clearing this cache when it can make a cached row id physically
  maintained.
