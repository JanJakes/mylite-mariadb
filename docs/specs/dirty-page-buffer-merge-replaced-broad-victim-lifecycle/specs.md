# Dirty-Page Buffer Merge Replaced Broad Victim Lifecycle

## Problem

The replaced-broad-victim direct-write path now reports the incoming leaf and
the would-be pressure victim it preserves. That proves the decision-time victim
shape, but it does not show whether the preserved victim later receives more
in-buffer rewrites or simply flushes unchanged.

Further direct-write policy work needs lifecycle evidence for the preserved
resident page before widening or narrowing the predicate.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting only.
- `packages/mylite-storage/src/storage.c`:
  - `dirty_page_buffer_merge_entry_should_direct_write_for_replaced_broad_victim()`
    has the incoming entry and would-be pressure victim at direct-write
    decision time.
  - `store_dirty_page_in_buffer_at_pressure_write_site()` is the shared dirty
    buffer admission and replacement path, so it can observe later rewrites of
    a tagged preserved victim.
  - `flush_statement_dirty_page_buffer()` and `flush_dirty_page_buffer_entry()`
    are the dirty-buffer flush paths that can report a tagged preserved
    victim's final replacement state.
  - `tag_dirty_page_buffer_entry_merge_fallback_origin()` already resets
    test-hook admission metadata when a dirty-buffer slot is reused.
- `tools/mylite_perf_baseline.c` prints the direct-write matrix added by the
  previous slice and the rejected-candidate fallback matrices this evidence
  will inform.

## Design

Add test-hook-only lifecycle metadata to dirty-page buffer entries preserved by
the replaced-broad-victim direct-write path:

- incoming leaf free-slot detail at the direct-write decision;
- preserved victim leaf free-slot detail at the direct-write decision;
- preserved victim replacement state at the direct-write decision.

The first time an untagged resident victim is preserved, the test hook records a
lifecycle start and stores those slots on the resident dirty-buffer entry. Later
same-page replacements increment a lifecycle replacement counter keyed by that
initial shape. Dirty-buffer flush paths record the final replacement state by
flush source. The direct-write decision matrix remains event-based; lifecycle
start rows are resident-entry based, so repeated direct writes preserving the
same tagged victim do not overwrite the original lifecycle tag.

## Compatibility Impact

No SQL behavior, public MyLite C API, handler API, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to route through
MyLite. All new state and accessors are test-hook-only.

## Single-File And Lifecycle Impact

No durable state, sidecars, journal layout, rollback layout, or embedded
lifecycle changes. The lifecycle tag is process-local dirty-buffer metadata in
test-hook builds and is reset when the dirty-buffer slot is reused.

## Public API And File Format Impact

No public API or on-disk format changes. New symbols are internal test-hook
accessors used by the storage self-test and benchmark tool.

## Storage-Engine Routing Impact

No routing change. Supported MySQL/MariaDB storage-engine names, including
`ENGINE=InnoDB`, continue to resolve through the MyLite storage layer.

## Binary-Size Impact

No new dependencies. The slice adds a few test-hook fields on dirty-buffer
entries, heap-backed lifecycle counters, focused storage assertions, and
benchmark output.

## Tests And Verification

- Extend the focused replaced-broad-victim direct-write self-test to assert:
  - one lifecycle start for the synthetic preserved victim;
  - one later replacement event after rewriting that preserved victim;
  - one final test-hook flush row with the preserved victim's final replacement
    state.
- Extend the prepared-insert benchmark with preserved-victim lifecycle start,
  replacement, and flush tables.
- Current storage-smoke evidence for
  `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  - prepared insert step: `71.562 us/op`;
  - replaced-broad-victim direct writes: `2,747` dirty `index-leaf` pages;
  - lifecycle starts: `1,152` preserved resident victims;
  - later preserved-victim replacement events: `465`;
  - buffer-limit flushes: `1,138` tagged preserved victims;
  - `32-63` incoming leaves started lifecycle tags on `245` replaced-once and
    `184` replaced-multiple `32-63` victims, plus `133` replaced-once and `87`
    replaced-multiple `64-127` victims;
  - `64-127` incoming leaves started lifecycle tags on `220` replaced-once and
    `75` replaced-multiple `32-63` victims, plus `145` replaced-once and `63`
    replaced-multiple `64-127` victims;
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

- The benchmark reports lifecycle starts, later replacement events, and flush
  states for victims preserved by the replaced-broad-victim direct-write path.
- Existing merge direct-write, fallback replay, flush, rollback, and
  pressure-selection behavior is unchanged.
- Focused storage tests cover lifecycle tagging, replacement accounting, and
  flush accounting.

## Risks

- Lifecycle start rows count resident dirty-buffer entries, while the existing
  direct-write matrix counts direct-write decisions. If the same victim is
  preserved by multiple direct writes before it changes, those totals can
  intentionally differ.
