# Dirty-Page Buffer Merge Fallback Replacement Counters

## Problem

The committed policy direct-writes full, near-full, and `16-31` free-slot
future-current index leaves during child dirty-buffer merge. A bounded
`32-63` direct-write experiment reduced dirty leaf pressure admissions but
regressed prepared insert from `68.775 us/op` to `72.554 us/op`, suggesting
that some wider partial leaves still benefit from parent dirty-buffer
coalescing.

The current counters show the remaining fallback rows by guard outcome and
free-slot detail, and they show global dirty-buffer replacements. They do not
connect a merged fallback leaf to later replacements before pressure or commit
flush. Without that link, the next policy cannot tell which `32+` free-slot
classes are truly cold enough to publish directly.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice only changes first-party MyLite storage test hooks and benchmark
  reporting in `packages/mylite-storage/src/storage.c` and
  `tools/mylite_perf_baseline.c`; no upstream MariaDB handler or SQL source is
  changed.
- `merge_dirty_page_buffer()` computes a direct-write guard outcome for each
  child dirty-page entry before either direct-writing it or replaying it into
  the parent dirty-page buffer.
- `store_dirty_page_in_buffer()` records per-entry `replacement_count` under
  test hooks, and the replacement path already classifies leaf changes as
  append, insert, same-shape, shrink, or other.
- Flush counters can report a buffered leaf's final replacement state when it
  leaves the dirty-page buffer, but they do not know whether that entry came
  from dirty-buffer merge fallback or which guard/free-slot class admitted it.

## Design

Add test-hook-only origin metadata to dirty-page buffer entries admitted by
merge fallback:

- direct-write guard outcome at admission;
- admitted leaf free-slot detail band.

Use that metadata to publish two counter families:

- merge fallback leaf replacement events by guard outcome, admitted free-slot
  detail, and leaf change class;
- merge fallback leaf flush replacement states by flush source, guard outcome,
  admitted free-slot detail, and final replacement state.

The counters are observational. They do not change direct-write eligibility,
dirty-buffer replacement behavior, page layout, rollback, journaling, or file
format. Entries not admitted through merge fallback remain untagged.

## Compatibility Impact

No SQL syntax, public C API, handler API, metadata, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to resolve through the
MyLite storage engine.

## Single-File And Lifecycle Impact

No files or durable state are introduced. The new metadata lives only in
test-hook builds and only inside in-memory dirty-page buffer entries.

## Public API And Binary Impact

No public API changes and no dependencies. Test-hook builds gain extra counter
storage and exported test-hook accessors. Non-test-hook builds do not carry
the new fields or counters.

## Tests And Verification

- Add a focused storage self-test that inserts a `future-current-header-partial-leaf`
  merge fallback entry into a full parent dirty buffer, replaces that entry
  before flush, and asserts the new replacement-event and flush-state counters.
- Extend prepared-insert component benchmark output with the new nonzero
  tables.
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

- Test-hook builds can attribute merge fallback leaf replacements to the guard
  outcome and free-slot detail band that admitted the buffered entry.
- Test-hook builds can report whether those merge fallback entries were never,
  once, or multiply replaced by the time they flush.
- Prepared-insert benchmark output reveals whether the remaining `32+`
  future-current fallback rows are being coalesced in the parent dirty buffer.
- No committed direct-write behavior changes.

## Verification Evidence

VPS prepared-insert component evidence after implementation:

- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  reported the new merge fallback replacement tables.
- `future-current-header-partial-leaf` fallback rows with `32-63` free slots
  recorded `5,324` replacement events: `94` append events and `5,230` insert
  events. Their flush states were `15,576` never replaced, `1,792` replaced
  once, and `923` replaced multiple times.
- `future-current-header-partial-leaf` fallback rows with `64-127` free slots
  recorded `5,895` replacement events: `79` append events and `5,816` insert
  events. Their flush states were `12,427` never replaced, `1,063` replaced
  once, and `578` replaced multiple times.
- `future-current-header-partial-leaf` fallback rows with `128+` free slots
  recorded `17,298` replacement events: `4,274` append events and `13,024`
  insert events. Their flush states were `1,404` never replaced, `105`
  replaced once, and `276` replaced multiple times.
- The same smoke run retained the committed behavior counters: `53,136` dirty
  `index-leaf` merge direct writes, `34,484` dirty `index-leaf` pressure
  admissions, `115,619` branch entry-count fast replacements, `13,922` branch
  entry-count fence fast replacements, and `33,851` leaf growth fast
  replacements.

## Risks

- The counters add test-hook bookkeeping around a hot test path, so benchmark
  values should be compared only against other test-hook benchmark runs.
- Replacement evidence does not by itself prove a new publication policy; it
  only identifies candidate classes for a later bounded behavior slice.
