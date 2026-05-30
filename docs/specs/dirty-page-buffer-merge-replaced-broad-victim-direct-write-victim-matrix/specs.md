# Dirty-Page Buffer Merge Replaced Broad Victim Direct-Write Victim Matrix

## Problem

The replaced-broad-victim direct-write policy reports the incoming pages through
the existing merge direct-write guard outcome tables. That proves how often the
policy fires and the incoming leaf free-slot bands, but it does not report the
would-be pressure victim that was preserved in the parent dirty buffer.

Follow-up policy work needs to know which victim shapes the behavior actually
protects after feedback from earlier direct writes changes the parent dirty
buffer sequence.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting only.
- `packages/mylite-storage/src/storage.c`:
  - `dirty_page_buffer_merge_entry_should_direct_write_for_replaced_broad_victim()`
    selects the new direct-write policy and has both the incoming entry and
    would-be pressure victim available.
  - `dirty_page_buffer_pressure_flush_index()` identifies the victim that would
    be flushed if the incoming page entered the parent dirty buffer.
  - `dirty_page_buffer_entry_is_replaced_broad_leaf()` defines the preserved
    victim predicate.
  - `dirty_page_buffer_flush_leaf_replacement_state()` and
    `dirty_page_buffer_leaf_free_slot_detail_band()` classify victim state for
    existing test-hook output.
- `tools/mylite_perf_baseline.c` already prints the direct-write guard outcome
  tables and rejected-candidate pressure-victim matrices used by this slice.

## Design

Add a test-hook-only matrix for the replaced-broad-victim direct-write path:

- incoming leaf free-slot detail;
- preserved victim leaf free-slot detail;
- preserved victim replacement state;
- direct-write page count.

The matrix is recorded only when the new direct-write predicate fires. It reads
existing in-memory page metadata and dirty-buffer replacement counters; it does
not change the direct-write predicate, pressure-selection policy, or file
format.

The prepared-insert benchmark prints nonzero rows after the direct-write guard
tables, before the fallback rejected-candidate matrices.

## Compatibility Impact

No SQL behavior, public MyLite C API, handler API, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to route through
MyLite. The new function is a test-hook-only benchmark helper.

## Single-File And Lifecycle Impact

No durable state, sidecars, journal layout, rollback layout, or embedded
lifecycle changes. The new matrix is process-local test-hook state and is reset
with the existing prepared-insert profile reset path.

## Public API And File Format Impact

No public API or on-disk format changes. The new symbol is an internal
test-hook accessor used by storage self-tests and the local benchmark tool.

## Storage-Engine Routing Impact

No routing change. Supported MySQL/MariaDB storage-engine names, including
`ENGINE=InnoDB`, continue to resolve through the MyLite storage layer.

## Binary-Size Impact

No new dependencies. The slice adds one heap-backed test-hook matrix, one
summary accessor, focused storage assertions, and benchmark output.

## Tests And Verification

- Extend the focused replaced-broad-victim direct-write storage self-test so the
  synthetic direct-write reports one matrix cell for incoming `32-63` free slots
  preserving a replaced-once `32-63` victim.
- Extend the prepared-insert benchmark output with the replaced-broad-victim
  direct-write preserved-victim matrix.
- Current storage-smoke evidence for
  `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  - prepared insert step: `76.832 us/op`;
  - replaced-broad-victim direct writes: `2,747` dirty `index-leaf` pages;
  - incoming `32-63` leaves preserved `421` replaced-once `32-63` victims,
    `663` replaced-multiple `32-63` victims, `257` replaced-once `64-127`
    victims, and `210` replaced-multiple `64-127` victims;
  - incoming `64-127` leaves preserved `412` replaced-once `32-63` victims,
    `285` replaced-multiple `32-63` victims, `325` replaced-once `64-127`
    victims, and `174` replaced-multiple `64-127` victims;
  - dirty leaf pressure admissions: `31,979`;
  - direct dirty leaf merge writes: `55,902`;
  - residual rejected below-tail admissions: `9,619`.
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

- The benchmark reports the preserved-victim matrix for the replaced-broad
  direct-write path.
- Existing merge direct-write, fallback replay, flush, rollback, and
  pressure-selection behavior is unchanged.
- Focused storage tests and prepared-insert benchmark output cover the new
  matrix accessor.

## Risks

- The matrix describes only the preserved victim at guard-evaluation time. It
  does not prove the preserved victim will later coalesce enough to repay the
  direct write; future behavior changes still need benchmark validation.
