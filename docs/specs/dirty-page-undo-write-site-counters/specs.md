# Dirty Page Undo Write Site Counters

## Problem

After routing maintained branch read paths through active caches, the
prepared-insert benchmark has no dirty pager-read copy rows. The remaining
dirty-page-copy refreshes are under dirty-page undo capture:

- `dirty-page-undo-capture / index-leaf`: `3,075` checksum-dirty copies.
- `dirty-page-undo-capture / index-branch`: `243` checksum-dirty copies.

The context tells us the refreshes happen while capturing before images for
writes, but not which writer path is responsible. Before changing undo capture
or dirty-buffer lifecycle behavior, the benchmark needs write-site evidence.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `pager_write_page()` calls `capture_dirty_page_undo_for_pager_write()`
  before writing durable page bytes.
- `capture_dirty_page_undo_for_pager_write()` reads the current page through
  `read_page_at()`, so an active dirty-buffer entry can force a
  dirty-page-copy refresh.
- Current counters classify those copies as `dirty-page-undo-capture` by page
  family, but do not identify the `pager_write_page()` caller.

## Design

Add test-hook dirty-page undo write-site counters:

- Thread-locally record the caller function name while `pager_write_page()` is
  executing.
- Keep existing dirty-page copy context and pager-read site counters unchanged.
- When `copy_dirty_page_buffer()` records a dirty-buffer hit under
  `dirty-page-undo-capture`, also record total and checksum-dirty copies by
  write site and page family.
- Use a fixed-size test-hook site table with borrowed `__func__` strings; do
  not allocate memory or change runtime storage structures.
- Print a compact non-zero write-site/family table in `mylite_perf_baseline`.

This slice is observability-only. It does not change undo capture, dirty-buffer
replacement, write ordering, or journaling behavior.

## Affected Subsystems

- MyLite storage test hooks.
- Prepared-insert performance benchmark output.
- Dirty-page undo-capture observability.

No MariaDB SQL-layer or handler-layer code changes are planned.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. The slice only records test-hook counters
around existing pager writes.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Journal, rollback, dirty
buffer, active statement, and pager lifetimes are unchanged.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API, durable file-format, dependency, or license change.
Binary-size impact is limited to test-hook builds and the development
benchmark tool. The fixed site table avoids per-write allocations.

## Test And Verification Plan

- Add storage test-hook coverage proving a dirty-buffer pager write records the
  caller function name, page family, and checksum-dirty state while capturing
  undo.
- Run the prepared-insert benchmark and compare write-site/family dirty counts
  with the existing `dirty-page-undo-capture` dirty context totals.
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

## Verification Results

Run on the VPS continuation environment with GCC 14.2:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `361.08 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` size was `33,970,634` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `341.35 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; prepared insert step was `78.928 us/op`.

The benchmark printed non-zero undo-capture write-site rows:

- `redistribute_branch_index_leaf_range_entry / index-leaf`: `3,000` copies,
  `3,000` dirty copies.
- `split_branch_index_leaf_entry / index-leaf`: `75` copies, `75` dirty
  copies.
- `split_branch_index_leaf_entry / index-branch`: `384` copies, `243` dirty
  copies.
- `write_maintained_index_root_overflow_flags / index-root`: `2` copies, `0`
  dirty copies.

The dirty write-site counts explain the benchmark's dirty-page-undo-capture
dirty context totals: `3,075` dirty `index-leaf` copies and `243` dirty
`index-branch` copies.

## Acceptance Criteria

- Dirty-page undo-capture copy hits record write site, page-family, and
  checksum-dirty counts for `pager_write_page()` callers.
- The prepared-insert benchmark prints non-zero undo write-site/family rows.
- Write-site dirty counts explain the existing dirty-page-undo-capture dirty
  context totals for the benchmark.
- Existing storage behavior and compatibility tests pass.

## Risks

The first pass attributes `pager_write_page()` callers. Other direct undo
capture entry points may need their own site tagging later if they become hot
in a different benchmark phase.
