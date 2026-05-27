# Prepared Update Path Counters

## Problem

The prepared update component benchmark now reports whether the hot loop uses
statement-scoped storage wrappers, but that evidence stops at the wrapper
boundary. The next update slices need to know whether each mutation used active
buffered rewrite, inline update append pages, fallback append pages, or
maintained index-root work before changing the storage path.

This matters on the current VPS because platform profiling tools such as
`sample`, `perf`, and `gdb` are unavailable. Test-hook counters give repeatable
local evidence without depending on stack sampling.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` routes
  accepted direct updates to MyLite storage update wrappers.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries_for_context()`
  chooses between maintained-root planning, active buffered rewrite, inline
  update append pages, and the fallback row/state/index append path.
- `packages/mylite-storage/src/storage.c::rewrite_active_update_pages()`
  reports active rewrite success through `used_active_update_rewrite`.
- `tools/mylite_perf_baseline.c` already prints prepared update wrapper
  counters under `MYLITE_STORAGE_TEST_HOOKS`.

## Design

Extend the existing prepared-update test-hook counter set with storage decision
counters:

- maintained-root update planning and non-empty update/retarget plans,
- active rewrite attempts and successes,
- active row-only and active single-index rewrite successes,
- active rewrite skips caused by maintained-root work,
- inline update writes,
- fallback append update writes.

The counters remain thread-local test hooks. They do not alter production
storage behavior, public APIs, or file format. The performance harness prints
them in the existing prepared update counter table for
`prepared-update-components`, `prepared-assignment-update-components`,
`prepared-row-only-update-components`, and miss variants.

## Affected Subsystems

- MyLite storage test-hook instrumentation.
- Storage-smoke performance baseline output.
- Focused storage tests for active statement update scope.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, or wire-protocol behavior
changes. The counters are compiled only with storage test hooks and do not
change MySQL/MariaDB compatibility claims.

## Single-File And Lifecycle Impact

No durable `.mylite` file-format, journal, lock, recovery, companion-file, or
embedded lifecycle change.

## Public API And File-Format Impact

No public `libmylite` API change and no durable file-format change. The added
accessors are private test-hook symbols used by first-party tests and tools.

## Binary-Size And Dependency Impact

No new dependency. Production builds without test hooks do not carry the
counter state or output.

## Tests And Verification

- Extend `test_active_statement_update_row_scope()` to prove active row-only
  rewrite, inline write, and maintained-root skip counters are reset and
  updated consistently with existing wrapper counters.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 100000`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-row-only-update-components 1000 100000`

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  no formatting changes.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  1/1 test in 298.83 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline
  mylite_storage_test mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, 2/2 tests in 309.11 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 1000 100000`: prepared update step
  measured `238.573 us/op`; counters reported `100000` statement-scope
  indexed-row reads, `100000` changed-index statement-scope writes, `100000`
  maintained-root update plan checks, `100000` active rewrite attempts,
  `99000` active rewrite successes, `98000` active single-index rewrite
  successes, `1000` inline update writes, and `0` append update writes.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 1000 100000`: prepared row-only
  update step measured `254.788 us/op`; counters reported `100000`
  statement-scope indexed-row reads, `100000` preserving-index statement-scope
  writes, `100000` active rewrite attempts, `99000` active row-only rewrite
  successes, `1000` inline update writes, and `0` append update writes.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 100000`: passed with prepared insert
  step `299.967 us/op` and packed-tail missing-page blockers `2`.

## Acceptance Criteria

- Prepared update benchmark output identifies storage decision counts below the
  wrapper level.
- Existing storage and embedded storage-engine tests pass.
- The counter reset function clears both wrapper and decision counters.
- The diff is limited to test-hook instrumentation, benchmark output, focused
  test assertions, and documentation.

## Risks And Unresolved Questions

- Counters are evidence, not an optimization by themselves. Follow-up slices
  still need to use the reported shape to choose a bounded behavior change.
- Maintained-root counters intentionally count non-empty plans, not every
  catalog probe. If a future slice needs catalog-read accounting, it should add
  a separate narrowly named counter.
