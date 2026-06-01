# Dirty Pressure Page-Type Cache

## Problem

Prepared-insert dirty-buffer pressure scans still classify resident buffer
entries as index leaves by reading the page type from page bytes. Dirty-buffer
entries already cache the page type after every admission and replacement, and
pressure scans are repeated many times while the buffer is full.

This is redundant writer-side metadata work. It is not validation, checksum
publication, journal validation, maintained-root planning, or durable-reader
verification.

## Source Findings

- Target base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage only:
  `packages/mylite-storage/src/storage.c`.
- Dirty-buffer entries store cached page type via
  `refresh_dirty_page_buffer_entry_page_type()` after admission and
  replacement.
- `dirty_page_buffer_pressure_flush_index()` and
  `dirty_page_buffer_pressure_complete_flush_context()` scan resident entries
  and currently call `is_index_leaf_page(buffer->entries[index].page)`.
- The existing `dirty_page_buffer_entry_is_index_leaf()` helper uses cached
  entry page type when present and falls back to the page bytes otherwise.

## Design

Add a small pressure-scan helper that calls
`dirty_page_buffer_entry_is_index_leaf()` and, in test-hook builds, records
when a cached page-type fact satisfied the probe. Use that helper in the two
dirty-buffer pressure scan loops.

Do not change pressure-victim selection, dirty-buffer flushing, direct-write
classification, checksum refresh timing, protected-page validation, or fallback
behavior for entries without cached page type.

## Affected Subsystems

- MyLite storage dirty page buffer pressure selection.
- MyLite storage test-hook performance counters.
- Storage performance baseline reporting.

No SQL, handler, metadata, public API, storage-engine routing, or wire-protocol
behavior changes.

## Compatibility Impact

No user-visible behavior changes. MySQL/MariaDB compatibility evidence is
unchanged because this slice only removes an internal page-byte metadata probe.

## Single-File And Lifecycle Impact

No file-format, journal, recovery, sidecar, lock, or lifecycle behavior changes.
The helper reads transient dirty-buffer entry facts maintained from the same
in-memory page image.

## Public API, File Format, Routing, And Dependencies

- Public API impact: none.
- File-format impact: none.
- Storage-engine routing impact: none.
- Wire-protocol impact: none.
- Binary-size impact: one small static helper and test-hook counter.
- License/dependency impact: none.

## Test And Verification Plan

- Add focused storage self-test coverage that proves pressure page-type probes
  use cached dirty-buffer entry facts.
- Extend the prepared-insert benchmark pressure-context output with cached
  pressure page-type probe count.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - storage-smoke build, storage-smoke tests, MariaDB static smoke build
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Dirty-buffer pressure scan leaf checks use cached entry page-type facts when
  available.
- Entries without cached page type still fall back through the existing helper.
- The prepared-insert benchmark reports a nonzero cached pressure page-type
  probe count.
- Structural checksum, maintained-root decode, pressure admission, direct-write,
  and pressure-context counters stay unchanged.

## Risks And Unresolved Questions

- The cached page type must stay synchronized with entry page bytes. Existing
  admission and replacement paths already call
  `refresh_dirty_page_buffer_entry_page_type()` immediately after changing the
  page image; this slice relies on that invariant rather than adding a new one.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `332.05 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `348.00 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size was `33,997,754` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed under unrelated high host load (`16.44`, `16.55`, `17.76`). The
  sampled prepared-insert step was `73.443 us/op`.

The comparable prepared-insert benchmark reported `542,656` cached pressure
page-type probes and kept the structural counters unchanged: `8` full-page
checksum calls, `127,063` zero-tail checksum calls, `5` protected
maintained-root decodes, `21,031` dirty leaf pressure admissions, `66,144`
merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
pressure-context builds, `19,053` planned stores, and `13,004` cached victim
free-slot reads.
