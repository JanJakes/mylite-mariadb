# Dirty Buffer Entry Family Facts

## Problem

The current prepared-insert profile still spends high-volume work in
dirty-buffer merge and pressure paths. The structural counters are dominated
by `66,144` merge direct writes, `21,031` pressure admissions, and `125,212`
non-leaf branch guard rows. Earlier slices carried scoped page-family and leaf
occupancy facts through individual recorders, but each dirty-buffer entry still
forces family classification from page bytes when the merge guard, flush
recorders, and pressure-victim accounting need the same fact.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite dirty-buffer bookkeeping in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own this
  storage layer.
- `store_dirty_page_in_buffer()` is the central admission and replacement path
  for dirty-buffer entries. It already copies the full page into the entry
  after pressure flushes, planned pressure stores, and same-page replacements.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` currently
  classifies each child entry from page bytes before recording guard facts and
  then separately proves leaf identity before direct-write decisions.
- `flush_dirty_page_buffer_entry_with_leaf_page_id_rank_and_facts()` and
  pressure-victim accounting accept precomputed family facts, but their callers
  still derive those facts from the victim entry page bytes.
- `dirty_page_buffer_merge_broad_victim_guard_outcome()` is only reached after
  the merge guard has proved the incoming entry is an index leaf and has
  computed a valid free-slot count.

## Design

In test-hook builds, store a page-family fact on each dirty-buffer entry when
the entry page is admitted or replaced. Reuse that stored family in:

- merge guard fact initialization;
- flush page-family recording when no narrower precomputed family was passed;
- pressure-victim family recording for planned and unplanned pressure flushes.

Keep fallback page-family parsing for any entry without a stored family fact,
which protects direct tests and future callers that construct entries manually.

In both test-hook and non-test builds, remove the duplicate incoming leaf check
from the broad-victim guard context initializer. Its only caller already
proved leaf identity and computed incoming leaf free slots before calling it.

Do not change direct-write policy, pressure victim selection, checksum timing,
dirty-buffer replacement semantics, page bytes, journal protection, recovery
validation, maintained-root planning, or file format.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, metadata,
transaction, recovery, or compatibility support status changes. Dirty-buffer
publication policy and persisted bytes stay unchanged.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-buffer entries still publish through the same
flush and direct-write paths, with the same checksum refresh and journal
protection requirements.

## Binary Size And Dependency Impact

No dependencies are added. Test-hook builds add one cached page-family field
and validity bit to dirty-buffer entries. Non-test builds only remove a
duplicate leaf check in the broad-victim context initializer.

## Tests And Verification Plan

- Add focused self-test coverage proving dirty-buffer entry family facts are
  set at admission and updated after replacement.
- Existing dirty-buffer merge, pressure, flush, rollback, and recovery tests
  cover the unchanged publication behavior.
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

- Dirty-buffer entry family facts are set for newly admitted entries and update
  when an existing entry is replaced by another page family.
- Prepared-insert structural counters stay unchanged: merge direct writes,
  pressure admissions, dirty-refresh counts, checksum call counts,
  maintained-root decodes, pressure-context builds, planned stores, and
  future-page relation rows.
- Storage and embedded storage-engine smoke tests pass.

## Verification Results

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `build/dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed in `311.88 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed in `429.13 sec` (`414.12 sec` storage capabilities,
  `15.00 sec` embedded storage engine).
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  passed with archive size `33,989,146` bytes and `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`
  sampled prepared insert step at `66.763 us/op`.

The prepared-insert structural profile stayed unchanged: `8` full-page
checksum calls, `227,063` zero-tail checksum calls, `94,033` dirty refreshes,
`21,031` dirty leaf pressure admissions, `66,144` dirty leaf merge direct
writes, `87,176` index-leaf dirty refreshes, `677` maintained-root decodes,
`31,938` pressure-context builds, `19,053` planned stores, `122,388`
future-page relation rows, and `121` rejected below-tail candidate admissions.

## Risks

- Cached family facts must be refreshed after every entry page mutation. Keep
  parser fallback for entries without an initialized fact.
- The broad-victim duplicate leaf-check removal depends on the current single
  caller proving leaf identity first; any future caller must preserve that
  precondition or reintroduce an explicit check.
