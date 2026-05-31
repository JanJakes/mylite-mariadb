# Merge Guard Leaf Occupancy Facts

## Problem

The prepared-insert merge direct-write guard now reuses guard facts across
guard-outcome and future-page relation counters, but the guard fact builder
still computes index-leaf occupancy for every guarded page. The current profile
reports `130,983` non-leaf guard rows (`670` `index-root` parent-not-full rows,
`5,101` `index-branch` parent-not-full rows, and `125,212` `index-branch`
non-leaf rows). Those pages can never contribute leaf fill-band or free-slot
rows, yet test-hook builds still call `dirty_page_buffer_index_leaf_occupancy()`
for them.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`; upstream MariaDB does not own these
  dirty-buffer merge guard counters.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` fills
  `mylite_storage_test_dirty_page_buffer_merge_guard_facts` before guard
  decision checks. In test-hook builds it computes the page family and then
  unconditionally computes index-leaf occupancy.
- `record_dirty_page_buffer_merge_direct_write_guard_outcome()` consumes the
  precomputed family and occupancy when present, otherwise it falls back to
  parsing the page itself.
- The guard decision still checks `is_index_leaf_page(entry->page)` before
  using leaf free-slot state for future-current pages. That production decision
  check must not change.

## Design

Keep the guard decision logic, direct-write policy, and non-test build behavior
unchanged. In `MYLITE_STORAGE_TEST_HOOKS` builds, compute the guard page family
first, initialize an explicit non-leaf occupancy fact, and only compute real
leaf occupancy when the family is `index-leaf`.

Set `has_occupancy` for both real leaf occupancy and explicit non-leaf
occupancy so guard-outcome recording consumes the provided fact instead of
falling back to `dirty_page_buffer_index_leaf_occupancy()` for non-leaf pages.
The actual guard decision continues to validate leaf identity through the
existing `is_index_leaf_page()` check and only uses occupancy free slots when
the occupancy is leaf-valid.

Do not change future-page relation calculation, append-buffer checks, resident
page checks, undo checks, pressure-context planning, journaling, validation,
checksum bytes, or maintained-root planning.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format, or
compatibility support status changes. This is test-hook-only accounting work.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-buffer merge decisions, direct writes,
buffered writes, statement rollback, and storage file bytes stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing guard path.

## Tests And Verification

- Existing storage self-tests cover merge direct-write guard outcome family,
  leaf fill-band, free-slot, and free-slot detail counters.
- Verified:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
    passed in `331.79 sec`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
    passed in `340.67 sec`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
    produced a static archive of `33,989,146` bytes with `478` members
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The prepared-insert benchmark reported `78.593 us/op` for the prepared insert
step. Merge guard outcome rows stayed unchanged, including `4,455`
parent-not-full `index-leaf`, `670` parent-not-full `index-root`, `5,101`
parent-not-full `index-branch`, `3,827` full future-current direct-write
`index-leaf`, `31,312` near-full future-current direct-write `index-leaf`,
`18,120` `16-31` future-current direct-write `index-leaf`, `21,031`
future-current partial `index-leaf`, `125,212` non-leaf `index-branch`, and
`30,758` parent-resident `index-leaf` rows. Guard leaf fill-band, free-slot,
and free-slot detail rows stayed unchanged.

The structural counters stayed unchanged: `8` full-page checksum calls,
`227,063` zero-tail checksum calls, `677` maintained-root decodes, `87,178`
dirty-page-flush checksum refreshes, `21,031` pressure admissions, `66,144`
merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
pressure-context builds, `19,053` planned stores, and `122,388` future-page
relation rows.

## Acceptance Criteria

- Merge guard outcome rows stay unchanged, including `670` parent-not-full
  `index-root` rows, `5,101` parent-not-full `index-branch` rows, `125,212`
  non-leaf `index-branch` rows, and the existing leaf outcome rows.
- Guard leaf fill-band, free-slot, and free-slot detail rows stay unchanged.
- Future-page relation, replacement, publication, checksum, maintained-root
  decode, pressure-admission, merge direct-write, pressure-context, and planned
  store counters stay unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- An explicit non-leaf occupancy fact must only suppress test-counter fallback
  parsing. The guard decision itself must keep using the existing leaf identity
  and leaf-free-slot checks.
