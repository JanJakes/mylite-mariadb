# Dirty-Page Buffer Merge Replaced Broad Victim Lifecycle Exits

## Problem

The replaced-broad-victim direct-write lifecycle counters show `1,152`
first-time preserved victims and `1,138` buffer-limit flushes in the current
prepared-insert smoke profile. That leaves a small but important accounting gap:
tagged victims can leave the dirty-page buffer without a lifecycle flush if the
entry is explicitly discarded or the dirty buffer is cleared.

Before changing direct-write policy again, the benchmark should classify those
non-flush exits so lifecycle starts can be reconciled against all dirty-buffer
exit paths.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test hooks, storage self-tests,
  benchmark reporting, and documentation only.
- `packages/mylite-storage/src/storage.c`:
  - `record_dirty_page_buffer_merge_replaced_broad_victim_direct_write()` tags
    the resident victim preserved by the direct-write decision.
  - `flush_statement_dirty_page_buffer()` records tagged victims that leave via
    statement or buffer-limit flush and then resets the dirty buffer count.
  - `flush_dirty_page_buffer_entry()` records tagged victims that are flushed at
    one index before that slot is reused by pressure admission.
  - `discard_dirty_page_buffer_entry()` removes a single dirty-buffer entry
    without flushing it.
  - `clear_dirty_page_buffer()` frees all dirty-buffer entries for rollback,
    cleanup, and statement teardown paths.
- `tools/mylite_perf_baseline.c` already prints preserved-victim lifecycle
  starts, later replacements, and flush states for the prepared-insert
  component profile.

## Design

Add test-hook-only preserved-victim lifecycle exit counters for tagged entries
that leave without a lifecycle flush:

- discard counts, recorded by `discard_dirty_page_buffer_entry()` before the
  matching entry is unlinked and any last entry is moved into its slot;
- clear counts, recorded by `clear_dirty_page_buffer()` before dirty-buffer
  storage is freed.

Both counters are keyed by the lifecycle tag captured at direct-write decision
time: incoming leaf free-slot detail, preserved victim leaf free-slot detail,
and preserved victim initial replacement state. They reuse the same slot
validation helper as replacement and flush counters, keeping invalid or untagged
entries invisible.

The benchmark prints two new tables after preserved-victim flush states:

- preserved victim discards;
- preserved victim clears.

## Compatibility Impact

No SQL behavior, public MyLite C API, handler API, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to route through
MyLite. All new state and symbols are test-hook-only.

## Single-File And Lifecycle Impact

No durable state, sidecars, journal layout, rollback layout, or embedded
lifecycle changes. The new counters observe existing dirty-buffer exit paths
without changing when pages are flushed, discarded, cleared, or restored.

## Public API And File Format Impact

No public API or on-disk format changes. The new accessors are internal
test-hook symbols used by the storage self-test and performance baseline tool.

## Storage-Engine Routing Impact

No routing change. Supported MySQL/MariaDB storage-engine names, including
`ENGINE=InnoDB`, continue to resolve through the MyLite storage layer.

## Binary-Size Impact

No new dependencies. Test-hook builds gain two small heap-backed counter
tensors and benchmark output loops.

## Tests And Verification

- Add a focused storage self-test that tags synthetic dirty-buffer entries and
  asserts:
  - `discard_dirty_page_buffer_entry()` records one discard and no clear;
  - `clear_dirty_page_buffer()` records one clear and no discard.
- Extend the prepared-insert benchmark with preserved-victim discard and clear
  tables.
- Current storage-smoke evidence for
  `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  - prepared insert step: `78.695 us/op`;
  - replaced-broad-victim direct writes: `2,747` dirty `index-leaf` pages;
  - lifecycle starts: `1,152` preserved resident victims;
  - buffer-limit flushes: `1,138` tagged preserved victims;
  - discards: `14` tagged preserved victims;
  - clears: `0` tagged preserved victims;
  - lifecycle starts reconcile exactly as `1,138` flushes plus `14`
    discards and `0` clears.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Tagged preserved victims that exit by discard or clear are counted by initial
  lifecycle shape.
- The benchmark can reconcile lifecycle starts against flushes plus non-flush
  exits.
- Existing merge direct-write, fallback replay, flush, rollback, and
  pressure-selection behavior is unchanged.

## Risks

- A dirty-buffer entry flushed by the single-entry test-hook flush helper is not
  removed by that helper. The production pressure path immediately reuses and
  retags that slot, but focused tests must reset counters or clear tags before
  teardown if they intentionally flush one entry and then clear the buffer.
