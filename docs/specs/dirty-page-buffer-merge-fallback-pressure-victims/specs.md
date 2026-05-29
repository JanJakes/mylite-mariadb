# Dirty-Page Buffer Merge Fallback Pressure Victims

## Problem

Future-current partial index leaves with `32+` free slots still fall back into
the parent dirty-page buffer during child dirty-buffer merge. A bounded
below-tail direct-write experiment was rejected because it regressed the
prepared insert step, but the current counters only describe the incoming
fallback leaves and their eventual replacement/flush state. They do not show
which buffered page was evicted at the instant one of those fallback leaves
forced buffer-limit pressure.

The next policy decision needs that join: if a rejected below-tail candidate
mostly evicts cold leaves, direct publication may only move work earlier; if it
evicts hot coalescing victims, the pressure selector or direct-write policy
needs a different predicate.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting only.
- `merge_dirty_page_buffer()` already installs test-hook context for fallback
  leaf admissions before replaying the child dirty entry into the parent:
  direct-write guard outcome, parent leaf rank, and parent tail-distance band.
- `store_dirty_page_in_buffer_at_pressure_write_site()` selects one victim with
  `dirty_page_buffer_pressure_flush_index()` when the parent dirty-page buffer
  is full, flushes that entry, then replaces the slot with the incoming page.
- `flush_dirty_page_buffer_entry()` already records global victim family,
  shape, page-id rank, and replacement-state counters, but those counters are
  not tied to the incoming merge fallback entry that caused pressure.
- The existing merge guard/fallback counter tensor is heap-backed behind one
  lazy test-hook thread-local pointer, so the new counters can live there
  without increasing static TLS.

## Design

Add test-hook-only counters recorded immediately after pressure chooses a
victim and before the victim is flushed:

- incoming fallback leaf guard outcome;
- incoming fallback leaf parent tail-distance band;
- incoming fallback leaf free-slot detail band;
- victim page family and checksum-dirty state;
- victim leaf replacement state when the victim is an index leaf.

Expose focused accessors for the tail-distance pressure-victim counters and
summary accessors for the previously rejected below-tail direct-write
candidate:

- guard outcome is `future-current-header-partial-leaf`;
- parent leaf tail distance is `below-parent-max-by-32-127`;
- admitted free-slot detail is `32-63` or `64-127`.

The prepared-insert benchmark prints compact rejected-candidate victim tables
for page family and leaf replacement state. Full existing tail-distance,
replacement, flush, pressure incoming, and pressure victim tables remain
unchanged.

## Compatibility Impact

No SQL behavior, public MyLite C API, handler API, storage-engine routing, or
file-format behavior changes. `ENGINE=InnoDB` continues to route through
MyLite. The new functions are test-hook-only benchmark helpers.

## Single-File And Lifecycle Impact

No durable state, sidecars, journal layout, rollback layout, or embedded
lifecycle changes. The counters are process-local test-hook state and are reset
with the existing prepared-insert profile reset path.

## Public API And File Format Impact

No public API or on-disk format changes. The new symbols are internal test-hook
accessors used by storage self-tests and the local benchmark tool.

## Storage-Engine Routing Impact

No routing change. Supported MySQL/MariaDB storage-engine names, including
`ENGINE=InnoDB`, continue to resolve through the MyLite storage layer.

## Binary-Size Impact

No new dependencies. The added code is limited to test-hook builds and the
benchmark tool. Counter storage is allocated lazily on the heap only when the
existing test-hook tensor is used.

## Tests And Verification

- Extend the focused dirty-page buffer merge fallback parent-rank/tail-distance
  storage self-test so the synthetic rejected below-tail candidate reports:
  - one pressure victim in the `index-leaf` family;
  - zero checksum-dirty victim pages for the clean synthetic tail victim;
  - one `never-replaced` victim leaf.
- Extend the prepared-insert benchmark output with rejected below-tail
  pressure-victim family and replacement-state summaries.
- Implementation evidence on `custom-storage`:
  - dev `mylite-storage` CTest passed in `287.16 sec`;
  - embedded static smoke build completed with `libmariadbd.a` at
    `33,974,138` bytes;
  - storage-smoke CTests passed, including `mylite-storage` in `384.13 sec`
    and `libmylite.embedded-storage-engine` in `28.38 sec`;
  - prepared-insert benchmark reported a `75.525 us/op` prepared insert step,
    `53,136` dirty leaf direct merge writes, and `34,484` dirty leaf pressure
    admissions;
  - rejected below-tail candidate admissions remained `11,971`, with `24`
    append replacements, `2,191` insert replacements, and buffer-limit flush
    states of `11,538` never replaced, `185` replaced once, and `238`
    replaced multiple times;
  - rejected below-tail pressure-victim output reported all `11,971` victims as
    checksum-dirty `index-leaf` pages, with leaf replacement states of `10,637`
    never replaced, `802` replaced once, and `532` replaced multiple times.
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

- Buffer-limit pressure caused by a merge fallback leaf records the selected
  victim before the slot is overwritten.
- Rejected below-tail candidate summary accessors can report victim family,
  checksum-dirty family, and leaf replacement-state counts.
- Existing merge direct-write and fallback behavior is unchanged.
- Focused storage tests and prepared-insert benchmark output cover the new
  evidence.

## Risks

- The pressure-victim join only records the victim chosen at buffer-limit
  pressure. It does not replace the broader flush tables, which remain the
  source of full statement-commit and later replacement evidence.
- The rejected-candidate summary intentionally hides detail that remains
  available in the full tail-distance tables.
