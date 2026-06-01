# Dirty Branch Fast Replacement Facts

## Problem

Prepared inserts still perform `130,311` branch replacement fast paths in the
dirty buffer (`115,753` entry-count-only, `14,172` entry-count-plus-fence, and
`386` child-insert replacements). Those helpers have already proved the
resident and incoming pages are table index branch pages and mutate the
resident page into the incoming branch image for the narrow changed fields.

After a successful branch fast replacement, `store_dirty_page_in_buffer()` still
runs `refresh_dirty_page_buffer_entry_page_type()`, which rereads the page type
and clears leaf-fill facts from the page bytes. That is redundant writer-side
metadata work. It is not checksum publication, journal validation, recovery
validation, maintained-root planning, or protected-page validation.

## Source Findings

- Target base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage only:
  `packages/mylite-storage/src/storage.c`.
- `replace_dirty_page_buffer_branch_entry_count_or_fences()` rejects non-branch
  pages, invalid branch metadata, key-size changes, child-count changes,
  used-byte changes, changed child page ids, and unstable tail bytes before
  applying entry-count-only or entry-count-plus-fence updates.
- `replace_dirty_page_buffer_branch_child_insert()` rejects non-branch pages,
  invalid branch metadata, non-single-child growth, changed fixed metadata, and
  non-insert/split-shaped payload changes before applying the branch child
  insert.
- Dirty-buffer entries already cache page-type facts after admission or
  replacement, and branch fast replacements do not change the page type.

## Design

Add a branch fact publication helper that marks a dirty-buffer entry as a table
index branch page, marks index-leaf fill facts as known-invalid, clears cached
leaf-fill fields, and clears maintained-root facts. After a branch fast
replacement helper succeeds, publish those facts and let
`store_dirty_page_in_buffer()` skip the generic page-type refresh for that
replacement.

Keep all other replacement paths on the existing generic refresh. Do not change
the branch proof helpers' byte checks, checksum fields, checksum-dirty timing,
dirty-buffer pressure policy, direct-write guards, or maintained-root fast
replacement behavior.

Add a test-hook counter for successful branch fast replacement fact
publications and print it in the prepared-insert benchmark beside the branch
fast replacement counters.

## Affected Subsystems

- MyLite storage dirty page buffer replacement.
- MyLite storage test-hook performance counters.
- Storage performance baseline reporting.

No SQL, handler, metadata, public API, storage-engine routing, or wire-protocol
behavior changes.

## Compatibility Impact

No user-visible behavior changes. Eligible replacements produce the same
resident dirty-buffer page image as before; only redundant cached metadata
refresh work is skipped.

## Single-File And Lifecycle Impact

No file-format, journal, recovery, sidecar, lock, or lifecycle behavior
changes. The cached facts describe the same transient dirty-buffer branch page
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

- Extend branch entry-count, entry-count-plus-fence, and child-insert fast
  replacement self-tests to assert successful fast replacements publish cached
  branch page-type facts and increment the new publication counter.
- Assert a non-fast structural branch replacement keeps the publication counter
  at zero after reset.
- Extend the prepared-insert benchmark storage counter output with cached branch
  fast replacement fact publications.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - storage-smoke build, storage-smoke tests, MariaDB static smoke build
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Successful branch fast replacements publish cached dirty-buffer page-type
  facts and known-invalid leaf-fill facts.
- The generic page-type refresh is skipped for those branch fast paths and
  retained for non-fast replacements, maintained-root replacements, and generic
  full-page copies.
- The prepared-insert benchmark reports cached branch fast replacement fact
  publications matching the successful branch fast replacement total.
- Structural checksum, maintained-root decode, pressure admission,
  direct-write, dirty-refresh, and pressure-context counters do not increase.

## Risks And Unresolved Questions

- The fast path must not hide branch shape validation. This slice only publishes
  facts after the existing branch proof helpers return success.
- Maintained-root replacement facts are intentionally left on the existing
  refresh path because maintained-root protected-page validation and root facts
  have separate safety constraints.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `321.69 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `349.72 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size was `33,998,098` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed under high unrelated host load (`18.81`, `19.11`, `19.61`). The
  sampled prepared-insert step was `76.215 us/op`.

The comparable prepared-insert benchmark reported `130,311` cached branch fast
replacement fact publications, matching the branch fast replacement total:
`115,753` entry-count, `14,172` entry-count-plus-fence, and `386`
child-insert replacements. Structural counters stayed unchanged: `8` full-page
checksum calls, `127,063` zero-tail checksum calls, `5` protected
maintained-root decodes, `21,031` dirty leaf pressure admissions, `66,144`
merge direct writes, `87,176` index-leaf dirty refreshes, `31,938`
pressure-context builds, `19,053` planned stores, `13,004` cached victim
free-slot reads, `542,656` cached pressure page-type probes, and `34,548`
cached leaf-growth fact publications.
