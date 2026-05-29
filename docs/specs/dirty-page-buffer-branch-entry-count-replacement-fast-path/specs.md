# Dirty Page Buffer Branch Entry Count Replacement Fast Path

## Problem

The prepared-insert replacement change-class profile now shows that most
dirty-buffer branch rewrites only change the page-owned branch entry-count
field. The latest 100,000-iteration run after `165f500b` reported `115,619`
entry-count-only branch replacements, `13,922` entry-count-plus-fence branch
replacements, and no structural branch replacements.

The current dirty-buffer replacement path copies the full 4 KiB page over the
resident dirty-buffer slot even when the only non-checksum change is the
8-byte branch entry-count field. That keeps correctness simple, but it makes
the hottest maintained-branch rewrite class pay for unnecessary page copying.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`, as recorded in
  `docs/architecture/engineering-standards.md`.
- This slice changes first-party MyLite storage code only:
  `packages/mylite-storage/src/storage.c`,
  `packages/mylite-storage/tests/storage_test.c`, and
  `tools/mylite_perf_baseline.c`.
- `store_dirty_page_in_buffer_at_pressure_write_site()` replaces an existing
  resident dirty-buffer page with `memcpy(existing->page, page, 4096)`.
- Branch insert refresh code updates branch cells and entry counts before
  calling `pager_write_maintained_insert_page()`. For the dominant profile
  class, the resident page and incoming page are identical except for the
  branch checksum field and `MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET`.
- Branch pages have fixed offsets for checksum and entry count, so a
  conservative byte comparison can detect entry-count-only replacement without
  decoding or refreshing checksums.

## Design

Add a production dirty-buffer replacement fast path for resident branch pages
when the incoming branch page differs only in:

- `MYLITE_STORAGE_FORMAT_INDEX_BRANCH_CHECKSUM_OFFSET`; and
- `MYLITE_STORAGE_FORMAT_INDEX_BRANCH_ENTRY_COUNT_OFFSET`.

When the fast path matches, update only those two 8-byte fields and the
resident `checksum_dirty` flag instead of copying the full page. All existing
replacement counters still record the replacement against the final resident
page image. Non-branch replacements, branch fence changes, structural branch
changes, invalid branch metadata, and first admissions keep the existing full
page-copy path.

Test-hook builds add a counter for applied fast-path replacements so the local
prepared-insert benchmark can confirm coverage.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, or durable bytes change. The fast path writes
the same resident dirty-buffer page image as the full copy for the eligible
entry-count-only branch replacement class.

## Single-File And Lifecycle Impact

No files are introduced. Journal, rollback, pressure flush, statement commit,
and dirty-buffer lifecycle behavior remain unchanged.

## Binary Size And Dependency Impact

No new dependencies. Test-hook builds add one counter accessor. Production
builds add one conservative branch replacement helper and avoid full page
copies only for byte-proven entry-count-only branch replacements.

## Tests And Verification

- Add a storage test-hook case that replaces a resident branch page with an
  entry-count-only update and verifies:
  - the resident branch page has the incoming entry count and checksum field;
  - the fast-path counter increments;
  - existing replacement family, branch-level, and branch-change counters still
    report the replacement.
- Verify a branch fence change still takes the full replacement path and does
  not increment the fast-path counter.
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

- Entry-count-only branch replacements update the resident dirty-buffer page
  without a full 4 KiB copy.
- Branch fence and structural replacements keep the existing full copy path.
- Dirty-buffer replacement counters, flush behavior, rollback behavior, and
  checksum-dirty semantics remain correct.
- The prepared-insert benchmark reports the number of applied fast-path branch
  replacements.

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

The post-implementation prepared-insert benchmark reported `59.042 us/op` for
the prepared insert step and `115,619` branch entry-count fast replacements.
It also preserved the previously observed branch replacement shape:
`115,619` entry-count-only branch replacements, `13,922`
entry-count-plus-fence replacements, and no structural branch replacements.

## Risks

- The fast path depends on fixed branch page offsets. The helper must compare
  all page bytes except the checksum and entry-count fields before applying the
  narrow update.
