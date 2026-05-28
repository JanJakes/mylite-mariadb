# Dirty Page Copy Context Counters

## Problem

Prepared-insert benchmark evidence still reports dirty-buffer copy refreshes:

- `dirty-page-copy / index-leaf`: `3,075`;
- `dirty-page-copy / index-branch`: `4,464`.

The source/family counters prove the refreshes happen while copying dirty
buffered pages for reads, but they do not say why the read occurred. The next
optimization depends on whether these copies are caused by undo capture before
direct writes, ordinary pager reads in maintained-index paths, or direct
storage reads outside the pager.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `copy_dirty_page_buffer()` is called from `read_page_at()` before active read
  snapshots, append buffers, or file IO.
- `pager_read_page()` is the common maintained-index read wrapper.
- `capture_dirty_page_undo_for_pager_write()` also calls `read_page_at()` when
  it must capture a before image for a direct pager write.
- Current counters only classify the copied page family, not the read context.

## Design

Add test-hook dirty-page copy context counters:

- `direct-read`: `read_page_at()` reached without a more specific context;
- `pager-read`: `read_page_at()` reached through `pager_read_page()`;
- `dirty-page-undo-capture`: `read_page_at()` reached while capturing a dirty
  page undo before a pager write.

Record total copied pages and checksum-dirty copied pages by context and page
family when `copy_dirty_page_buffer()` returns a dirty-buffer hit. Keep the
existing dirty-checksum refresh counters unchanged.

Expose the context names and counts through storage test hooks and print a
context/family table in `mylite_perf_baseline` near the dirty-refresh source
tables.

## Affected Subsystems

- MyLite storage test hooks.
- Prepared-insert performance benchmark output.
- Dirty-page copy observability.

No MariaDB SQL-layer or handler-layer code changes are planned.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. The slice only records test-hook counters
around existing read paths.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Journal, rollback, dirty
buffer, and active statement lifetimes are unchanged.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API, durable file-format, dependency, or license change.
Binary-size impact is limited to test-hook counters and benchmark output in the
development/test builds.

## Test And Verification Plan

- Add storage test-hook coverage for direct dirty-page copies and
  dirty-page-undo-capture copies.
- Run the prepared-insert benchmark and compare context/family counts against
  the existing `dirty-page-copy` family totals.
- Keep storage and routed embedded storage-engine tests passing.
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

- Dirty-page buffer copy hits record context, page-family, and checksum-dirty
  counts.
- The prepared-insert benchmark prints dirty-page copy context/family counts.
- Existing dirty-refresh source/family counters remain intact.
- Existing storage behavior and compatibility tests pass.

## Verification Results

The requested verification completed on the VPS:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`:
  passed in `309.10 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` is `33,970,026` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `313.78 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; prepared insert step measured `65.311 us/op`.

On the storage-smoke profile, the prepared-insert benchmark reports the
remaining checksum-dirty dirty-page-copy refreshes under pager reads:

- `pager-read / index-leaf`: `3,075` copies, `3,075` checksum-dirty copies.
- `pager-read / index-branch`: `7,687` copies, `4,464` checksum-dirty copies.
- `dirty-page-undo-capture / index-leaf`: `3,075` copies, `0`
  checksum-dirty copies.
- `dirty-page-undo-capture / index-branch`: `384` copies, `0`
  checksum-dirty copies.
- `direct-read / index-root`: `1,336` copies, `0` checksum-dirty copies.

The `dirty-page-copy` refresh source totals remain `3,075` index-leaf and
`4,464` index-branch refreshes, matching the pager-read checksum-dirty context
counts.

## Risks

If the context table shows most copies in broad `pager-read`, a follow-up slice
may need more focused maintained-index read-site tagging before changing code.
