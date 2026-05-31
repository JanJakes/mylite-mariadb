# Merge Guard Outcome Occupancy Facts

## Problem

Prepared-insert profiling records `122,388` future-page merge relation rows and
the corresponding merge direct-write guard outcome tables. The guard already
classifies incoming future index leaves by free-slot count to decide between
full, near-full, `16-31`, broad-victim direct writes, and fallback. The
test-hook guard outcome recorder then parses the same incoming leaf metadata
again to fill guard outcome fill-band and free-slot tables.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage test-hook accounting in
  `packages/mylite-storage/src/storage.c`.
- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` owns the
  merge direct-write guard decision and already computes incoming future-leaf
  free-slot counts before broad-victim policy.
- `record_dirty_page_buffer_merge_direct_write_guard_outcome()` records guard
  family, dirty-family, fill-band, free-slot, and free-slot-detail rows but
  recomputes `mylite_storage_test_index_leaf_occupancy` from the same page.
- The current benchmark reports `66,144` direct dirty leaf merge writes,
  `21,031` `future-current-header-partial-leaf` fallback rows, `31,938`
  pressure-context builds, and `19,053` planned stores. Those policy counters
  must remain unchanged.
- The remaining maintained-root decodes are planning, root-read, and
  recovery-journal validation sites and are not part of this slice.

## Design

Replace the test-hook-only future-relation out parameter with a small guard
facts object that carries:

- the existing future-page relation state; and
- the incoming page occupancy facts derived by the guard when profiling is
  enabled.

Use those facts in the guard outcome recorder when available. Keep the recorder
fallback that derives occupancy itself for direct tests and any future caller
that does not supply guard facts.

Do not thread these facts into pressure insertion or fallback-origin tagging in
this slice. Those paths mutate or admit dirty-buffer entries and remain a
separate safety boundary.

## Implementation Notes

Implemented in `packages/mylite-storage/src/storage.c` behind
`MYLITE_STORAGE_TEST_HOOKS`:

- `dirty_page_buffer_merge_direct_write_guard_outcome_for_entry()` now accepts
  a test-hook guard facts object instead of only the future-relation object.
- The guard facts object carries the existing future-page relation state and,
  when profiling is enabled, the incoming page occupancy classification.
- `record_dirty_page_buffer_merge_direct_write_guard_outcome()` consumes the
  precomputed occupancy when supplied and keeps the direct-test fallback that
  derives occupancy from the page.
- Non-test-hook builds keep the previous production guard signature and do not
  compile the guard facts object into the merge guard API.

## Compatibility Impact

No SQL behavior, public C API behavior, storage-engine routing, file format,
durable bytes, or compatibility support status changes. This is
`MYLITE_STORAGE_TEST_HOOKS` profiling-source work only.

## Single-File And Lifecycle Impact

No file lifecycle changes. Dirty-page publication, checksum refresh, pressure
selection, direct-write decisions, journaling, rollback, and statement commit
behavior stay unchanged.

## Binary Size And Dependency Impact

No dependencies are added. Non-test-hook builds keep the existing guard
signature and should not gain the guard facts object.

## Tests And Verification

- Existing storage self-tests cover merge direct-write guard outcome counters,
  future-page relation counters, and planned pressure fallback replay.
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

Final verification:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `316.07 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `317.29 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size `33,989,146` bytes, `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; prepared insert step sample `67.763 us/op`.

The benchmark kept the policy and publication counters unchanged: `122,388`
future-page relation rows, `66,144` dirty `index-leaf` merge direct writes,
`21,031` dirty `index-leaf` pressure admissions, `87,176` index-leaf dirty
refreshes, `31,938` merge pressure-context builds, `19,053` planned stores,
`8` full-page checksum calls, `227,063` zero-tail checksum calls, and `677`
maintained-root decodes. Maintained-root decode sites stayed limited to
`read_index_leaf_run_root` (`1`), `plan_maintained_index_root_inserts` (`674`),
and `validate_recovery_journal_saved_page` (`2`).

## Acceptance Criteria

- Merge direct-write guard outcome, fill-band, free-slot, free-slot-detail,
  and future-relation counters are unchanged.
- Prepared-insert checksum, maintained-root decode, pressure-admission, merge
  direct-write, and dirty-refresh counters are unchanged.
- Storage and embedded storage-engine tests pass.

## Risks

- Guard facts are evidence for the current child dirty-buffer entry only. They
  must not be reused after the entry is copied, admitted, flushed, or mutated.
