# Test-Hook Leaf Occupancy Classification

## Problem

Prepared-insert profiling still spends source-path work in test-hook
classification after the remaining maintained-root decodes have been narrowed
to planning, root-read, and journal-validation sites. The dirty-page flush,
pressure, and merge recorders repeatedly parse the same index-leaf occupancy
metadata to derive fill bands, free-slot bands, and free-slot detail bands for
one page.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- The slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; it does not modify upstream MariaDB
  source or handler semantics.
- The current storage benchmark keeps `677` maintained-root decodes at
  `read_index_leaf_run_root`, `plan_maintained_index_root_inserts`, and
  `validate_recovery_journal_saved_page`. Those are validation/planning gates
  and are outside this slice.
- `record_dirty_page_buffer_flush_page()` derives shape, fill band, free-slot
  band, and replacement matrices through separate occupancy reads.
- `record_dirty_page_buffer_pressure_incoming_page()` and
  `record_dirty_page_buffer_merge_direct_write_guard_outcome()` derive fill,
  free-slot, and detail bands separately for the same leaf.
- Merge fallback and broad-victim recorders also classify incoming and victim
  leaf free-slot detail independently from nearby fill/free-slot
  classification.

## Design

Add a test-hook-only leaf occupancy facts helper that reads the fixed-width
index-leaf metadata once and derives:

- fill band,
- free-slot band,
- free-slot detail band,
- entry count and capacity for dirty/full shape decisions.

Use that helper in dirty-page flush, pressure incoming, merge guard, merge
fallback origin, pressure-victim, broad-victim, and replacement leaf accounting.
Also merge the two flush page-id-rank recorders so rank and rank/fill-band
classification share one rank scan and one occupancy classification per flushed
leaf.

The helper returns the same invalid bands as the existing individual helpers
when a page is an index leaf but its occupancy metadata is not valid.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
durable bytes, catalog metadata, or supported/unsupported compatibility surface
changes. The change is compiled under `MYLITE_STORAGE_TEST_HOOKS` and only
affects profiling counters.

## Single-File And Lifecycle Impact

No files are introduced. Dirty-page flush order, journal protection, checksum
publication, rollback, and nested-statement merge semantics stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Production builds without storage test hooks do not
include the helper or the refactored accounting paths.

## Tests And Verification

- Keep the existing storage self-tests that assert flush fill-band,
  free-slot, page-id-rank/fill, replacement-state/fill, and
  replacement-state/free-slot counters.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Flush, pressure, and merge test-hook counters remain structurally unchanged.
- The prepared-insert maintained-root decode sites remain unchanged and still
  include planning/journal validation.
- Full-page and zero-tail checksum counters remain unchanged unless a later
  checksum-specific slice changes durable publication.
- Storage and embedded storage-engine smoke tests pass.

## Verification Evidence

On the VPS on 2026-05-31:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `322.61 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  was `33,989,146` bytes with `478` members.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `347.78 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed.

The prepared-insert structural profile stayed unchanged:

- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- maintained-root decodes: `677`
  (`read_index_leaf_run_root` `1`,
  `plan_maintained_index_root_inserts` `674`,
  `validate_recovery_journal_saved_page` `2`);
- dirty leaf pressure admissions: `21,031`;
- merge direct writes: `66,144`;
- index-leaf dirty refreshes: `87,176`;
- merge pressure-context builds: `31,938`;
- planned pressure stores: `19,053`;
- flush page-id rank rows preserved the `2` statement-commit non-leaf
  `invalid` rows.

The final benchmark sample reported a `79.149 us/op` prepared insert step on a
variable shared host, so wall-clock timing is recorded as host-load noisy rather
than a speed claim for this source-path slice.

## Risks

- This is a profiling-source-path slice, not a durable checksum-publication
  change. It should improve test-hook benchmark overhead but must not be used
  as evidence that remaining validation decodes are removable.
