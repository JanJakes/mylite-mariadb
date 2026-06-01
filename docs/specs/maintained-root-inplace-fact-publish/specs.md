# Maintained Root In-Place Fact Publish

## Problem

Prepared inserts now keep protected maintained-root validation down to the
remaining journal and durable-read gates, but the in-place maintained-root
writer still republishes dirty-buffer root facts by rereading the mutated page
header after a successful root insert or overflow-tail mark.

That reread is writer-side metadata refresh work. The same code path has
already validated the protected root page during planning or parent dirty-page
admission, and the mutator has just applied a narrow, validated update to the
current dirty-buffer page image.

## Source Findings

- Target base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage only:
  `packages/mylite-storage/src/storage.c`.
- `plan_maintained_index_root_inserts()` reads maintained-root pages through
  `read_maintained_index_root_plan_page()`. It decodes and validates protected
  pages when dirty-entry facts are unavailable.
- `insert_maintained_index_root_entry_in_dirty_buffer()` uses the planned root
  entry count and planned insertion position, then calls
  `insert_maintained_index_root_entry()` against the dirty page with deferred
  checksum publication.
- `mark_maintained_index_root_overflow_tail_in_dirty_buffer()` uses the
  planned overflow root state, validates parent dirty roots before cloning, and
  calls `mark_maintained_index_root_overflow_tail()` against the dirty page with
  deferred checksum publication.
- Both in-place writers call
  `refresh_dirty_page_buffer_entry_maintained_root_facts_from_valid_page()` after
  success, rereading maintained-root metadata from page bytes only to republish
  dirty-entry facts.

## Design

Add a dirty-buffer maintained-root fact publication helper. It sets the entry's
page-type facts to table index root, marks leaf-fill facts as known-invalid,
and stores the maintained-root table id, index number, key size, entry count,
used bytes, flags, and overflow-tail page id supplied by the validated writer
state.

After `insert_maintained_index_root_entry()` succeeds in the dirty-buffer
writer, publish:

- table id from the writer argument;
- index number and key size from the inserted index entry;
- entry count as the planned entry count plus one;
- used bytes from the root cell size and new entry count; and
- flags and overflow-tail page id from the already mutated page bytes.

After `mark_maintained_index_root_overflow_tail()` succeeds in the dirty-buffer
writer, publish:

- table id, index number, key size, entry count, and used bytes from the
  planned overflow root; and
- flags with `HAS_OVERFLOW_TAIL` set and overflow-tail page id from the
  fallback page id argument.

Keep durable reads, journal saved-page validation, recovery validation, parent
dirty-page admission validation, checksum publication, and non-in-place
fallback paths unchanged.

Add a test-hook counter for maintained-root fact publications and print it in
the prepared-insert benchmark beside the maintained-root fast replacement
counters.

## Affected Subsystems

- MyLite storage maintained-root dirty-buffer writer.
- MyLite storage dirty-buffer metadata facts.
- MyLite storage test-hook performance counters.
- Storage performance baseline reporting.

No SQL, handler, metadata, public API, storage-engine routing, or wire-protocol
behavior changes.

## Compatibility Impact

No user-visible behavior changes. Eligible in-place mutations produce the same
dirty-buffer root page image as before and keep the same checksum-dirty timing.
MySQL/MariaDB compatibility evidence is unchanged because this is internal
metadata-cache reuse.

## Single-File And Lifecycle Impact

No file-format, journal, recovery, sidecar, lock, or lifecycle behavior
changes. The cached facts describe the same transient dirty-buffer root page
image that the writer just mutated.

## Public API, File Format, Routing, And Dependencies

- Public API impact: none.
- File-format impact: none.
- Storage-engine routing impact: none.
- Wire-protocol impact: none.
- Binary-size impact: one small fact-publication helper and one test-hook
  counter/accessor.
- License/dependency impact: none.

## Test And Verification Plan

- Extend the maintained-root in-place insert self-test to assert successful
  fact publication and no maintained-root decode site when the plan rereads the
  dirty root.
- Extend the maintained-root in-place overflow-tail self-test to assert
  successful fact publication and correct cached overflow-tail facts.
- Extend the prepared-insert benchmark storage counter output with
  maintained-root cached fact publications.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - storage-smoke build, storage-smoke tests, MariaDB static smoke build
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Successful maintained-root in-place inserts and overflow-tail marks publish
  cached dirty-buffer maintained-root facts without calling the generic
  maintained-root fact refresh helper.
- Planning and journal/recovery protected-page validation remain unchanged.
- The prepared-insert benchmark reports maintained-root cached fact
  publications.
- Structural checksum, protected maintained-root decode, pressure admission,
  direct-write, dirty-refresh, and pressure-context counters do not increase.

## Risks And Unresolved Questions

- The cached facts must remain synchronized with page bytes. This slice only
  publishes after the existing mutation helper returns success, and focused
  tests validate the cached facts back against the page image.
- This does not remove protected maintained-root decodes or checksum
  publication. It removes a redundant writer-side metadata refresh after a
  successful in-place dirty-buffer mutation.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `328.38 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `352.31 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size was `33,997,842` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. The sampled prepared-insert step was `77.373 us/op`.

The comparable prepared-insert benchmark reported `668` maintained-root cached
fact publications, matching `666` maintained-root insert fast replacements and
`2` maintained-root overflow fast replacements. Structural counters stayed
unchanged: `8` full-page checksum calls, `127,063` zero-tail checksum calls,
`5` protected maintained-root decodes, `21,031` dirty leaf pressure admissions,
`66,144` merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
pressure-context builds, `19,053` planned stores, `13,004` cached victim
free-slot reads, `542,656` cached pressure page-type probes, `34,548` cached
leaf-growth fact publications, and `130,311` cached branch fast replacement
fact publications.
