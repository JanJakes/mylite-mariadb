# Merge Fallback Origin Leaf Facts

## Problem

The prepared-insert merge guard now carries explicit leaf and non-leaf
occupancy facts, but the fallback attribution path still checks
`is_index_leaf_page(entry->page)` and calls
`dirty_page_buffer_merge_fallback_parent_leaf_classification_for_entry()` for
every non-direct merge entry. For non-leaf entries, the classifier immediately
returns invalid after another leaf check. The current guard profile includes
`125,212` non-leaf `index-branch` rows plus `5,101` parent-not-full
`index-branch` rows and `670` parent-not-full `index-root` rows where fallback
origin leaf attribution must remain inactive.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own the
  dirty-buffer merge fallback attribution counters.
- `merge_dirty_page_buffer()` records guard outcome facts, then enters the
  fallback path when `dirty_page_buffer_merge_entry_should_direct_write()`
  returns false.
- The fallback path sets
  `test_dirty_page_buffer_merge_fallback_origin_active` from
  `is_index_leaf_page(entry->page)` and computes parent leaf page-id rank and
  tail distance with
  `dirty_page_buffer_merge_fallback_parent_leaf_classification_for_entry()`.
- The preceding slice made guard facts carry an explicit non-leaf occupancy
  fact, so the fallback path already has equivalent leaf/non-leaf state for
  test-hook builds.

## Design

In `MYLITE_STORAGE_TEST_HOOKS` builds, derive fallback origin activity from the
guard occupancy fact when it is available. If the guard fact says the entry is
not an index leaf, keep the fallback origin inactive and use the existing
invalid parent-leaf classification without calling the classifier.

Keep the old `is_index_leaf_page()` and classifier fallback for direct tests or
future callers that do not have guard occupancy facts. For real leaf entries,
continue to call the classifier so parent leaf page-id rank and tail-distance
rows are unchanged.

Do not change guard decisions, direct-write policy, pressure-context planning,
dirty-buffer storage, journaling, recovery validation, checksum bytes, or
maintained-root planning.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format, or
compatibility support status changes. This is test-hook-only attribution work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-buffer merge, fallback buffering, statement
rollback, and storage file bytes stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing merge path.

## Tests And Verification

- Existing storage self-tests cover fallback parent-rank, tail-distance,
  replacement, flush-state, discard, clear, rejected-candidate, and pressure
  victim rows.
- Verified:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
    passed in `360.31 sec`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
    passed in `383.05 sec`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
    produced a static archive of `33,989,146` bytes with `478` members
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The prepared-insert benchmark reported `104.250 us/op` for the prepared insert
step under variable host load. Fallback origin rows stayed unchanged, including
`21,031` future-current partial-leaf admissions, `121` rejected below-tail
candidate admissions, `6` rejected-candidate discards, `0` rejected-candidate
clears, and the same parent-rank, tail-distance, replacement, flush-state, and
pressure-victim rows.

The structural counters stayed unchanged: `8` full-page checksum calls,
`227,063` zero-tail checksum calls, `677` maintained-root decodes, `87,178`
dirty-page-flush checksum refreshes, `21,031` pressure admissions, `66,144`
merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
pressure-context builds, `19,053` planned stores, and `122,388` future-page
relation rows.

## Acceptance Criteria

- Fallback origin rows stay unchanged, including parent-rank, tail-distance,
  replacement, flush replacement-state, discard, clear, rejected-candidate, and
  pressure-victim tables.
- Guard outcome, future-page relation, replacement, publication, checksum,
  maintained-root decode, pressure-admission, merge direct-write,
  pressure-context, and planned-store counters stay unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- The fallback origin active flag must remain false for non-leaf entries. Only
  guard facts that explicitly identify a leaf may skip the old leaf check.
