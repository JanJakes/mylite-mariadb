# Dirty Page Buffer Pressure Admission Entry Replacement State

## Problem

Pressure admission-source counters show that the prepared-insert hot pressure
path is child-statement dirty-buffer merge, not direct dirty-buffer store. The
next policy question is whether those merge-sourced incoming pages already
benefited from coalescing in the child dirty buffer. If most incoming child
entries are never replaced before merge, a later merge-direct-write policy can
focus on first-admitted child pages instead of repeatedly rewritten child
entries.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test hooks and benchmark
  reporting in `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `merge_dirty_page_buffer()` sees each child dirty-buffer entry before it is
  replayed into the parent and can read the entry's test-hook replacement
  count without changing merge behavior.
- `record_dirty_page_buffer_pressure_incoming_page()` already records the page
  admitted after a buffer-limit flush. A test-hook thread-local state can carry
  the source child entry's replacement state into that existing record point.
- Direct stores do not have a dirty-buffer source entry, so they need an
  explicit `not-buffered-entry` bucket rather than reusing the merge entry
  states.

## Design

Add a test-hook-only pressure admission entry replacement-state counter:

- default pressure admissions to `not-buffered-entry`;
- while merging a child dirty-buffer entry, set the current admission entry
  state from the child entry's replacement count:
  `never-replaced`, `replaced-once`, or `replaced-multiple`;
- count incoming page family and checksum-dirty incoming page family by entry
  replacement state;
- expose slot-count, slot-name, count, and dirty-count accessors; and
- print a prepared-insert benchmark table after the pressure admission-source
  table.

This is a probe only. It does not bypass parent pressure admission or change
merge ordering.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, durable bytes, or supported/unsupported surface
changes. The new counters exist only when `MYLITE_STORAGE_TEST_HOOKS` is
enabled.

## Single-File And Lifecycle Impact

No files are introduced. The probe does not change dirty-page buffer capacity,
eviction order, direct-write fallback behavior, journal protection, rollback,
nested-statement isolation, page publication, checksum refresh timing, or
embedded open/close behavior.

## Binary Size And Dependency Impact

No new dependencies. Production builds without storage test hooks are
unchanged.

## Tests And Verification

- Add storage test-hook coverage proving:
  - direct pressure admission records `not-buffered-entry`;
  - merge pressure admission records `never-replaced`;
  - merge pressure admission records `replaced-once`; and
  - merge pressure admission records `replaced-multiple`.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Verification Evidence

The VPS storage-smoke benchmark reported an `80.660 us/op` prepared insert
step. All pressure admissions were sourced from child dirty-buffer entries that
had never been replaced in the child buffer:

| Buffered entry replacement state | Page family | Incoming pages | Checksum-dirty incoming pages |
| --- | --- | ---: | ---: |
| never-replaced | index-leaf | 85,257 | 85,257 |
| never-replaced | index-branch | 275 | 140 |

No `not-buffered-entry`, `replaced-once`, or `replaced-multiple` rows were
reported. The table totals `85,532` incoming pressure pages, matching the
buffer-limit pressure flush count and the existing admission-source table.

## Acceptance Criteria

- Prepared-insert benchmark output reports pressure admissions by source entry
  replacement state and page family.
- Existing pressure incoming, pressure admission-source, pressure write-site,
  flush, replacement, and checksum counters still report correctly.
- Storage and embedded storage-engine smoke tests pass.

## Risks

- Entry replacement-state counters identify whether incoming merge entries
  coalesced in the child buffer. They do not prove a merge direct-write policy
  is safe; that later change must still preserve rollback, nested-statement
  merge, journal protection, and active reader semantics with before/after
  benchmark evidence.
