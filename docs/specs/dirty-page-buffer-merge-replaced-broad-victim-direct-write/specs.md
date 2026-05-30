# Dirty-Page Buffer Merge Replaced Broad Victim Direct Write

## Problem

The rejected below-tail pressure-victim matrices show that most broad
future-current fallback leaves still evict broad leaves that were never replaced
in the parent dirty buffer. A smaller group is different: `1,194`
rejected-candidate pressure victims are broad `32-127` free-slot leaves that
were already replaced before pressure selected them. Evicting those victims can
discard useful in-buffer coalescing work.

The previous broad below-tail direct-write experiment regressed badly because it
wrote too many incoming pages. This slice tests a narrower behavior predicate:
direct-write only the rejected below-tail incoming page that would otherwise
evict an already-rewritten broad victim.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage behavior only inside the dirty
  page buffer merge fallback path.
- `packages/mylite-storage/src/storage.c`:
  - `merge_dirty_page_buffer()` computes the merge direct-write guard outcome
    before either writing the child entry directly or storing it in the parent
    dirty buffer.
  - `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` already
    permits future-current full, near-full, and `16-31` free-slot index leaves
    to direct-write when the parent buffer is full, the page is within the
    parent current header, and append-buffer/parent-resident guards pass.
  - `store_dirty_page_in_buffer_at_pressure_write_site()` selects the pressure
    victim with `dirty_page_buffer_pressure_flush_index()` before flushing and
    replacing that slot.
  - `dirty_page_buffer_pressure_flush_index()` already prefers clean leaves,
    then full dirty leaves, then the highest-fill non-tail dirty leaf.
  - `direct_write_dirty_page_buffer_merge_entry()` writes the child dirty page
    through the same checksum refresh and direct-write accounting path as the
    existing future-current direct-write outcomes.

## Design

Promote the dirty-buffer entry replacement counter from test-hook-only metadata
to runtime dirty-buffer metadata. It remains process-local, is initialized to
zero for first admissions, and increments when a resident dirty-buffer entry is
replaced.

Add one merge direct-write guard outcome:
`future-current-header-replaced-broad-victim-direct-write`.

The new outcome applies only when all of these are true:

- the existing guard would otherwise return
  `future-current-header-partial-leaf`;
- the incoming leaf has `32-63` or `64-127` free slots;
- the incoming page id is `32-127` pages below the parent dirty-buffer leaf
  tail;
- the parent dirty buffer is full and the current pressure victim is an index
  leaf;
- the victim leaf has `32-63` or `64-127` free slots;
- the victim dirty-buffer entry has been replaced at least once.

When the outcome fires, the incoming future-current page is written directly and
the already-rewritten victim remains in the parent dirty buffer. Existing
fallback behavior remains unchanged for all other candidates.

## Compatibility Impact

No SQL behavior, public MyLite C API, handler API, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to route through
MyLite. The behavior changes only which internal dirty pages are published
directly during nested dirty-buffer merge pressure.

## Single-File And Lifecycle Impact

No durable state, sidecars, journal layout, or embedded lifecycle changes. The
new runtime replacement counter is in-memory dirty-buffer metadata. The direct
write uses the existing future-current direct-write invariant: the page is
within the parent current header but beyond the parent statement header, is not
append-buffer resident, and has no previous durable state that needs an undo
record.

## Public API And File Format Impact

No public API or on-disk format changes.

## Storage-Engine Routing Impact

No routing change. Supported MySQL/MariaDB storage-engine names, including
`ENGINE=InnoDB`, continue to resolve through the MyLite storage layer.

## Binary-Size Impact

No new dependencies. The dirty-buffer entry keeps one runtime `unsigned`
replacement counter that was previously test-hook-only.

## Tests And Verification

- Extend the focused dirty-page buffer merge fallback parent-rank/tail-distance
  storage self-test to create an already-replaced broad victim and verify the
  new guard outcome direct-writes the incoming rejected below-tail candidate.
- Extend the prepared-insert benchmark output through existing guard outcome
  tables; no new benchmark table is required.
- Implementation evidence on `custom-storage`:
  - dev `mylite-storage` CTest passed in `293.75 sec`;
  - embedded static smoke build completed with `libmariadbd.a` at
    `33,974,986` bytes;
  - storage-smoke CTests passed, including `mylite-storage` in `362.64 sec`
    and `libmylite.embedded-storage-engine` in `22.83 sec`;
  - prepared-insert benchmark, sampled under unrelated concurrent host build
    load, reported an `80.670 us/op` prepared insert step, `55,902` dirty leaf
    direct merge writes, and `31,979` dirty leaf pressure admissions;
  - the new `future-current-header-replaced-broad-victim-direct-write` guard
    outcome fired for `2,747` dirty `index-leaf` pages: `1,551` in the `32-63`
    free-slot band and `1,196` in `64-127`;
  - rejected below-tail candidate admissions fell to `9,619`, and the residual
    admitted/victim free-slot/replacement-state matrix no longer reports
    already-replaced `32-127` free-slot victims.
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

- The new guard outcome fires only for the replaced-broad-victim predicate.
- Prepared-insert benchmark evidence shows the behavior is not another broad
  below-tail regression before the behavior is adopted.
- Existing merge fallback, direct-write rollback invariants, flush behavior, and
  pressure-selection behavior are unchanged outside the new predicate.
- Focused storage tests cover the direct-write branch and the non-matching
  fallback branch.

## Risks

- The predicate may still be too broad if preserving already-rewritten victims
  causes more random direct writes than the prepared-insert path can absorb. If
  benchmark evidence regresses materially, the behavior should not be adopted in
  this slice.
