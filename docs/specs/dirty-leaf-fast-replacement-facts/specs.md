# Dirty Leaf Fast Replacement Facts

## Problem

Prepared inserts still report `34,548` leaf growth fast replacements. Those
fast replacements already prove the resident index-leaf page and incoming page
differ by exactly one fixed-width cell insertion, and the helper computes the
new entry count, capacity, key size, and used-byte boundary while mutating the
resident dirty-buffer entry.

After a successful fast replacement, the caller still runs
`refresh_dirty_page_buffer_entry_page_type()`, which rereads the page type and
reparses index-leaf fill facts from the page bytes. That is redundant
writer-side metadata work. It is not checksum publication, journal validation,
recovery validation, maintained-root planning, or protected-page validation.

## Source Findings

- Target base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage only:
  `packages/mylite-storage/src/storage.c`.
- `store_dirty_page_in_buffer()` handles replacement of a dirty-buffer entry
  already resident for the same page id.
- `replace_dirty_page_buffer_leaf_single_insert()` rejects non-leaf pages,
  mismatched key sizes, invalid leaf metadata, non-single-entry growth, changed
  stable metadata, changed unused tail bytes, and payload changes that are not
  one inserted fixed-width leaf cell.
- On success, the helper mutates the resident page to the exact incoming page
  image for the leaf payload, entry count, used bytes, checksum field, and
  checksum-dirty flag.
- Dirty-buffer entries now cache page type and index-leaf fill facts after
  admission or replacement. Later pressure scans and dirty checksum refreshes
  can reuse those facts.

## Design

Publish cached dirty-buffer page-type and index-leaf fill facts directly from
`replace_dirty_page_buffer_leaf_single_insert()` after it has completed the
byte-proven mutation. The facts are:

- page type is table index leaf;
- key size is unchanged and already validated;
- entry count is the incoming entry count;
- entry capacity is the validated capacity for the key size; and
- used bytes are the incoming used-byte boundary.

When the fast path succeeds, `store_dirty_page_in_buffer()` treats the entry
facts as current and skips the generic
`refresh_dirty_page_buffer_entry_page_type()` pass. All other replacement paths
keep the existing generic refresh.

Add a test-hook counter for cached fact publications from successful leaf
growth fast replacements and print it in the prepared-insert benchmark beside
the existing fast replacement count.

## Affected Subsystems

- MyLite storage dirty page buffer replacement.
- MyLite storage test-hook performance counters.
- Storage performance baseline reporting.

No SQL, handler, metadata, public API, storage-engine routing, or wire-protocol
behavior changes.

## Compatibility Impact

No user-visible behavior changes. Eligible replacements produce the same
resident dirty-buffer page image as the existing fast path. MySQL/MariaDB
compatibility evidence is unchanged because this is an internal metadata-cache
reuse.

## Single-File And Lifecycle Impact

No file-format, journal, recovery, sidecar, lock, or lifecycle behavior
changes. The cached facts describe the same transient dirty-buffer page image
that the writer just mutated.

## Public API, File Format, Routing, And Dependencies

- Public API impact: none.
- File-format impact: none.
- Storage-engine routing impact: none.
- Wire-protocol impact: none.
- Binary-size impact: one small fact-publication helper and one test-hook
  counter/accessor.
- License/dependency impact: none.

## Test And Verification Plan

- Extend the existing leaf growth fast replacement storage self-test to assert
  that successful insert and append fast replacements publish valid cached leaf
  facts and increment the new publication counter.
- Assert a same-shape non-fast replacement keeps the publication counter at
  zero after reset.
- Extend the prepared-insert benchmark storage counter output with cached leaf
  growth fact publications.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - storage-smoke build, storage-smoke tests, MariaDB static smoke build
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Successful leaf growth fast replacements publish cached dirty-buffer
  page-type and index-leaf fill facts.
- The generic page-type/index-leaf-fill refresh is skipped for that fast path
  and retained for every other replacement path.
- The prepared-insert benchmark reports cached leaf growth fact publications
  matching the successful fast replacement count.
- Structural checksum, maintained-root decode, pressure admission,
  direct-write, dirty-refresh, and pressure-context counters do not increase.

## Risks And Unresolved Questions

- The fast path must keep the cached facts synchronized with the page bytes. It
  only publishes after the existing byte-equivalence proof and after mutating
  the resident page to the incoming image.
- This does not reduce checksum publication counts. It removes a redundant
  writer-side metadata refresh before later dirty-buffer paths reuse the same
  facts.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `385.67 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `355.09 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size was `33,997,970` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed under high unrelated host load (`17.87`, `21.67`, `21.42`). The
  sampled prepared-insert step was `93.185 us/op`.

The comparable prepared-insert benchmark reported `34,548` cached leaf-growth
fact publications matching `34,548` leaf growth fast replacements. Structural
counters stayed unchanged: `8` full-page checksum calls, `127,063` zero-tail
checksum calls, `5` protected maintained-root decodes, `21,031` dirty leaf
pressure admissions, `66,144` merge direct writes, `87,176` index-leaf dirty
refreshes, `31,938` pressure-context builds, `19,053` planned stores, `13,004`
cached victim free-slot reads, and `542,656` cached pressure page-type probes.
