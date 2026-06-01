# Dirty Buffer Entry Page Type Facts

## Problem

The current prepared-insert profile keeps high-volume dirty-buffer merge rows:
`125,212` `index-branch` non-leaf guard rows, `122,388` future-page relation
rows, `66,144` dirty leaf merge direct writes, and `21,031` dirty leaf
pressure admissions. The previous slice cached test-hook page-family facts,
but production and test-hook dirty-buffer code still rereads the page-type word
from each buffered entry for repeated index-leaf checks.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite dirty-buffer bookkeeping in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own this
  storage layer.
- `store_dirty_page_in_buffer()` is the only normal admission and replacement
  path for dirty-buffer entries. It copies or mutates the resident entry page
  before later merge, pressure, and flush checks inspect that entry.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()`,
  `dirty_page_buffer_merge_entry_leaf_free_slots()`, flush page-id rank
  attribution, fallback-origin attribution, and selected replacement helpers
  all re-run `is_index_leaf_page(entry->page)` even though they are checking the
  same resident dirty-buffer entry bytes.

## Design

Store a compact page-type fact on each dirty-buffer entry when the entry page
is admitted or replaced. Add dirty-buffer entry helpers that return the cached
page type when present and fall back to reading the page bytes for manually
constructed test entries.

Use those helpers for repeated entry-owned index-leaf checks in merge guard,
merge leaf free-slot calculation, flush rank classification, fallback-origin
recorders, and the existing leaf fast replacement gate. Keep checks against
incoming standalone page buffers unchanged.

Do not change direct-write policy, pressure victim selection, checksum timing,
dirty-buffer replacement semantics, page bytes, journal protection, recovery
validation, maintained-root planning, or file format.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, metadata,
transaction, recovery, or compatibility support status changes. The cached page
type is process-local dirty-buffer state only.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-buffer entries still publish through the same
flush and direct-write paths, with the same checksum refresh and journal
protection requirements.

## Binary Size And Dependency Impact

No dependencies are added. Dirty-buffer entries gain one page-type field and a
validity bit in all builds.

## Tests And Verification Plan

- Extend focused self-test coverage to prove dirty-buffer entry page-type facts
  are set at admission and updated after replacement.
- Existing dirty-buffer merge, pressure, flush, rollback, and recovery tests
  cover unchanged publication behavior.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Dirty-buffer entry page-type facts are set for newly admitted entries and
  update when an existing entry is replaced by another page type.
- Prepared-insert structural counters stay unchanged: merge direct writes,
  pressure admissions, dirty-refresh counts, checksum call counts,
  maintained-root decodes, pressure-context builds, planned stores,
  future-page relation rows, and non-leaf guard rows.
- Storage and embedded storage-engine smoke tests pass.

## Verification Results

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `build/dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in `380.38 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `364.11 sec` (`348.46 sec` storage capabilities,
  `15.64 sec` embedded storage engine).
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed with archive size `33,989,418` bytes and `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  sampled prepared insert step at `71.886 us/op`.

The prepared-insert structural profile stayed unchanged: `125,212`
`index-branch` non-leaf guard rows, `122,388` future-page relation rows, `8`
full-page checksum calls, `227,063` zero-tail checksum calls, `94,033` dirty
refreshes, `21,031` dirty leaf pressure admissions, `66,144` dirty leaf merge
direct writes, `87,176` index-leaf dirty refreshes, `677` maintained-root
decodes, `31,938` pressure-context builds, `19,053` planned stores, and `121`
rejected below-tail candidate admissions.

## Risks

- Cached page-type facts must refresh after every entry page mutation. Keep
  parser fallback for entries without an initialized fact.
- The helper must only replace checks against resident dirty-buffer entries;
  incoming standalone pages still need direct byte checks.
