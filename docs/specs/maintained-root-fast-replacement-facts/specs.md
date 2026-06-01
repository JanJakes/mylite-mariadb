# Maintained Root Fast Replacement Facts

## Problem

Maintained-root dirty-buffer fast replacements already prove narrow root page
changes before mutating the resident dirty-buffer entry, but
`store_dirty_page_in_buffer()` still treats those replacements as needing the
generic page-type refresh. That refresh rereads page-type metadata and clears
maintained-root facts even though the replacement helper has just proved and
applied a maintained-root page update.

The prepared-insert benchmark currently reports `668` checksum-dirty
maintained-root replacements (`666` single-entry inserts and `2` overflow-tail
marks). The dominant prepared-insert route now publishes facts through the
in-place writer, but the generic dirty-buffer replacement helpers still leave
the same fact-publication opportunity uncovered for other storage paths.

## Source Findings

- Target base: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice changes first-party MyLite storage only:
  `packages/mylite-storage/src/storage.c`.
- `replace_dirty_page_buffer_maintained_root_single_insert()` rejects non-root
  pages, key-size changes, invalid root metadata, non-single-entry growth,
  changed stable metadata, changed tail bytes, and payload changes that are not
  one inserted fixed-width root entry.
- `replace_dirty_page_buffer_maintained_root_overflow_tail()` rejects non-root
  pages, clean incoming pages, key-size changes, entry-count or used-byte
  changes, invalid root metadata, unsupported flags, existing overflow tails,
  unstable bytes outside the flags/overflow-tail/checksum fields, and non-dirty
  incoming checksums.
- After either helper succeeds, `store_dirty_page_in_buffer()` currently leaves
  `entry_facts_current` false, so the caller runs
  `refresh_dirty_page_buffer_entry_page_type()` and does not preserve
  maintained-root facts.

## Design

Reuse the maintained-root fact publication helper added for in-place root
writes. After the single-insert replacement helper mutates the resident root
page, publish table id, index number, key size, new entry count, new used
bytes, flags, and overflow-tail page id from the validated page image and
replacement inputs.

After the overflow-tail replacement helper mutates the resident root page,
publish table id, index number, key size, existing entry count, existing used
bytes, incoming flags, and incoming overflow-tail page id from the validated
replacement state.

Change `store_dirty_page_in_buffer()` so successful maintained-root
single-insert and overflow-tail fast replacements mark entry facts current and
skip the generic page-type refresh. Keep non-fast maintained-root replacements,
generic full-page copies, checksum publication, journal validation, recovery
validation, and planning protected-page validation unchanged.

Use the existing maintained-root cached fact publication counter for these
generic replacement publications too.

## Affected Subsystems

- MyLite storage dirty page buffer replacement.
- MyLite storage dirty-buffer metadata facts.
- MyLite storage test-hook performance counters.
- Storage performance baseline reporting.

No SQL, handler, metadata, public API, storage-engine routing, or wire-protocol
behavior changes.

## Compatibility Impact

No user-visible behavior changes. Eligible fast replacements produce the same
resident dirty-buffer root page image as before; only redundant cached metadata
refresh work is skipped.

## Single-File And Lifecycle Impact

No file-format, journal, recovery, sidecar, lock, or lifecycle behavior
changes. The cached facts describe the same transient checksum-dirty
dirty-buffer root page image that the replacement helper just produced.

## Public API, File Format, Routing, And Dependencies

- Public API impact: none.
- File-format impact: none.
- Storage-engine routing impact: none.
- Wire-protocol impact: none.
- Binary-size impact: no new dependency; this reuses the existing
  maintained-root fact publication helper and test counter.
- License/dependency impact: none.

## Test And Verification Plan

- Extend maintained-root single-insert replacement self-test to assert cached
  maintained-root facts and one fact publication.
- Extend maintained-root overflow-tail replacement self-test to assert cached
  maintained-root facts and one fact publication.
- Re-run the prepared-insert benchmark and verify structural counters do not
  increase.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - storage-smoke build, storage-smoke tests, MariaDB static smoke build
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Successful maintained-root single-insert and overflow-tail dirty-buffer
  replacements publish cached maintained-root facts.
- `store_dirty_page_in_buffer()` skips the generic page-type refresh for those
  two maintained-root fast paths and retains it for non-fast replacements.
- Planning and journal/recovery protected-page validation remain unchanged.
- Structural checksum, protected maintained-root decode, pressure admission,
  direct-write, dirty-refresh, and pressure-context counters do not increase.

## Risks And Unresolved Questions

- The replacement helpers must keep facts synchronized with page bytes. This
  slice only publishes after each existing byte-equivalence proof and mutation
  succeeds, and tests validate facts back against the page image.
- The prepared-insert benchmark counts both in-place writer publications and
  generic dirty-buffer replacement publications through the same scalar
  counter. Focused self-tests prove the generic replacement path, while the
  aggregate benchmark row remains a structural regression guard.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `331.35 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `339.01 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; archive size was `33,998,226` bytes with `478` members.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. The sampled prepared-insert step was `77.282 us/op`.

The comparable prepared-insert benchmark reported `1,336` maintained-root
cached fact publications: the prior `668` in-place writer publications plus
`668` dirty-buffer replacement publications matching `666` maintained-root
insert fast replacements and `2` maintained-root overflow fast replacements.
Structural counters stayed unchanged: `8` full-page checksum calls, `127,063`
zero-tail checksum calls, `5` protected maintained-root decodes, `21,031`
dirty leaf pressure admissions, `66,144` merge direct writes, `87,176`
index-leaf dirty refreshes, `31,938` pressure-context builds, `19,053` planned
stores, `13,004` cached victim free-slot reads, `542,656` cached pressure
page-type probes, `34,548` cached leaf-growth fact publications, and `130,311`
cached branch fast replacement fact publications.
