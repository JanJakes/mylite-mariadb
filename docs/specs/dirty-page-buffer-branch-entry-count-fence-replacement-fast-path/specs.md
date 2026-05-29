# Dirty Page Buffer Branch Entry Count Fence Replacement Fast Path

## Problem

After `7bff5e5f`, prepared-insert storage counters report that all
`115,619` entry-count-only branch replacements use the new dirty-buffer fast
path. The remaining branch replacement class is `13,922`
entry-count-plus-fence rewrites, while structural branch replacements remain
absent in the 100,000-iteration smoke profile.

Those replacements still copy the full 4 KiB branch page even though the
existing change-class classifier proves the branch shape is unchanged: same
key size, child count, used bytes, child page ids, and tail bytes, with changes
limited to the branch checksum, page-owned entry count, and child fence
row/key fields.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage code only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `dirty_page_buffer_replacement_branch_change_class()` already validates the
  entry-count-plus-fence class by comparing branch metadata, child page ids,
  per-child fence row/key fields, and unused tail bytes before assigning the
  `entry-count-and-fence` class.
- Branch cells store the child page id at offset `0`, the child max row id at
  offset `8`, and the child max key at offset `16`; the max key length is the
  branch key size.
- `store_dirty_page_in_buffer_at_pressure_write_site()` now tries the
  entry-count-only fast path before falling back to the full page copy.

## Design

Add a second production dirty-buffer replacement fast path for resident branch
pages when the incoming branch page differs only in:

- `MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHECKSUM_OFFSET`;
- `MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET`; and
- child fence row/key fields within existing branch cells.

The helper must reject pages with invalid branch metadata, changed key size,
changed child count, changed used bytes, changed child page ids, changed tail
bytes, or any changed non-fence metadata. When it matches, it updates the
resident checksum, entry count, and changed fence fields in place and records a
test-hook fast-path counter. Structural branch replacements keep the full page
copy path.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, durable file format, or persisted bytes change. Eligible
replacements produce the same resident dirty-buffer page image as the full
copy path.

## Single-File And Lifecycle Impact

No files are introduced. Journal, rollback, pressure flush, statement commit,
and dirty-buffer lifecycle behavior remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. Test-hook builds add one counter accessor. Production
builds add one branch replacement helper that reuses fixed branch-page offsets
and avoids full page copies for byte-proven entry-count-plus-fence branch
replacements.

## Tests And Verification

- Add a storage test-hook case that replaces a resident branch page with an
  entry-count-plus-fence update and verifies:
  - the resident branch page has the incoming entry count, checksum, and child
    fence;
  - the entry-count-plus-fence fast-path counter increments;
  - existing replacement family, branch-level, and branch-change counters still
    report the replacement.
- Verify a structural child-id change still takes the full replacement path and
  does not increment the new fast-path counter.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Entry-count-plus-fence branch replacements update the resident dirty-buffer
  page without a full 4 KiB copy.
- Structural branch replacements keep the existing full copy path.
- Dirty-buffer replacement counters, flush behavior, rollback behavior, and
  checksum-dirty semantics remain correct.
- The prepared-insert benchmark reports the number of applied
  entry-count-plus-fence fast replacements.

## Verification Evidence

VPS verification after implementation:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The post-implementation prepared-insert benchmark reported `65.730 us/op` for
the prepared insert step on this VPS. It reported `115,619` branch entry-count
fast replacements and `13,922` branch entry-count fence fast replacements,
matching the full observed branch change-class profile:
`115,619` entry-count-only replacements, `13,922` entry-count-plus-fence
replacements, and no structural replacements.

## Risks

- The fast path depends on fixed branch page and branch cell offsets. The
  helper must compare all page bytes except the checksum, entry-count, and
  existing child fence fields before applying the narrow update.
