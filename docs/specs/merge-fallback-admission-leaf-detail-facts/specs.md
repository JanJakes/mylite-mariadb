# Merge Fallback Admission Leaf Detail Facts

## Problem

The merge fallback origin path now uses guard occupancy facts to keep non-leaf
fallback origins inactive, but `tag_dirty_page_buffer_entry_merge_fallback_origin()`
still reparses active leaf entries with
`dirty_page_buffer_index_leaf_occupancy()` to recover the free-slot detail slot
used by fallback admission, replacement, flush, discard, clear, and pressure
victim counters.

In the prepared-insert smoke profile, the same guard fact already carries that
free-slot detail for fallback leaf entries before the page is stored in the
parent dirty buffer. The extra parse is redundant test-hook work.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook attribution in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  counters.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` fills
  `mylite_storage_test_dirty_page_buffer_merge_guard_facts::occupancy` for
  index-leaf entries.
- `merge_dirty_page_buffer()` already carries fallback-origin activity, guard
  outcome, parent leaf page-id rank, and parent tail-distance through
  thread-local test-hook state while storing fallback entries.
- `tag_dirty_page_buffer_entry_merge_fallback_origin()` currently calls
  `dirty_page_buffer_index_leaf_occupancy()` again only to populate
  `entry->merge_fallback_leaf_free_slot_detail_slot`.

## Design

Add a test-hook-only fallback-origin free-slot-detail slot to the existing
thread-local fallback-origin state. During merge fallback, populate it from the
guard occupancy fact when that fact identifies an index leaf. Use a sentinel
outside `MYLITE_STORAGE_TEST_DIRTY_PAGE_BUFFER_LEAF_FREE_SLOT_DETAIL_BAND_COUNT`
when no precomputed slot is available.

When tagging a stored fallback entry, use the carried slot if present. Keep the
old occupancy parse as a fallback for direct tests or future callers that set
fallback-origin activity without a guard fact.

Do not change guard decisions, pressure flushing, dirty-buffer storage,
journaling, recovery validation, checksum bytes, maintained-root planning, or
non-test builds.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format, or
compatibility support status changes. This is test-hook-only attribution work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-buffer merge, fallback buffering, statement
rollback, and durable storage bytes stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing merge path.

## Tests And Verification

- Existing storage self-tests cover fallback admission, replacement, flush,
  discard, clear, rejected-candidate, pressure-victim, parent-rank, and
  tail-distance counters.
- Verified:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
    passed in `315.35 sec`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
    passed in `341.75 sec`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
    produced a static archive of `33,989,146` bytes with `478` members
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The prepared-insert benchmark reported `73.973 us/op` for the prepared insert
step under variable host load. Fallback admission rows stayed unchanged,
including `21,031` future-current partial-leaf admissions split across
`11,623` `32-63`, `7,430` `64-127`, and `1,978` `128+` admitted leaf
free-slot detail rows. Fallback parent-rank, tail-distance, replacement,
flush-state, discard, clear, rejected-candidate, and pressure-victim rows
remained populated with the same structural distribution.

The structural counters stayed unchanged: `8` full-page checksum calls,
`227,063` zero-tail checksum calls, `677` maintained-root decodes, `87,178`
dirty-page-flush checksum refreshes, `21,031` pressure admissions, `66,144`
merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
pressure-context builds, `19,053` planned stores, and `122,388` future-page
relation rows.

## Acceptance Criteria

- Fallback admission, replacement, flush-state, discard, clear,
  rejected-candidate, pressure-victim, parent-rank, and tail-distance rows stay
  unchanged.
- Guard outcome, future-page relation, publication, pressure-admission,
  pressure-context, planned-store, checksum, and maintained-root decode counters
  stay unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- The carried free-slot-detail slot must distinguish a real `INVALID` detail
  band from "no precomputed slot." Use the count enum value as the unavailable
  sentinel, not the invalid band.
