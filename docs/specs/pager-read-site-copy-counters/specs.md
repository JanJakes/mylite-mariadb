# Pager Read Site Copy Counters

## Problem

The dirty-page copy context counters show that the prepared-insert benchmark's
remaining checksum-dirty copy refreshes are under the broad `pager-read`
context:

- `pager-read / index-leaf`: `3,075` checksum-dirty copies.
- `pager-read / index-branch`: `4,464` checksum-dirty copies.

That distinguishes pager reads from direct reads and dirty-page undo capture,
but it still does not identify which maintained-index read paths are forcing
the copies. The next optimization needs function-level attribution before
changing branch, leaf, or dirty-buffer behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts still reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `pager_read_page()` wraps `read_page_at()` for maintained-index branch,
  leaf, root, update, delete, metadata, and test paths.
- `copy_dirty_page_buffer()` records dirty-buffer copy hits when
  `read_page_at()` finds an active dirty page before active read snapshots,
  append buffers, or file IO.
- The storage-smoke prepared-insert benchmark after
  `8ca1da2d4e2e` shows all checksum-dirty dirty-page-copy refreshes under
  `pager-read`, while dirty-page undo capture copies are clean in that run.

## Design

Add test-hook pager-read site counters for dirty-page copy hits:

- Thread-locally record the caller function name while `pager_read_page()` is
  executing.
- Keep the existing context/family counters unchanged.
- When `copy_dirty_page_buffer()` records a dirty-buffer hit under the
  `pager-read` context, also record total and checksum-dirty copies by caller
  function and page family.
- Use a fixed-size test-hook site table with borrowed `__func__` strings; do
  not allocate memory or change runtime storage structures.
- Print a compact non-zero site/family table in `mylite_perf_baseline`.

This slice is intentionally observability-only. It does not change pager
read/write behavior, dirty-buffer replacement, maintained-index routing, or
checksum timing.

## Affected Subsystems

- MyLite storage test hooks.
- Prepared-insert performance benchmark output.
- Dirty-page copy observability for maintained-index reads.

No MariaDB SQL-layer or handler-layer code changes are planned.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. The slice only records test-hook counters
around existing in-process storage reads.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Journal, rollback, dirty
buffer, active statement, and pager lifetimes are unchanged.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API, durable file-format, dependency, or license change.
Binary-size impact is limited to test-hook builds and the development
benchmark tool. The fixed site table avoids per-read allocations.

## Test And Verification Plan

- Add storage test-hook coverage proving a dirty-buffer pager read records the
  caller function name, page family, and checksum-dirty state.
- Run the prepared-insert benchmark and compare site/family dirty counts with
  the existing `pager-read` dirty context totals.
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

- Pager-read dirty-buffer copy hits record caller site, page-family, and
  checksum-dirty counts.
- The prepared-insert benchmark prints non-zero pager-read site/family rows.
- Site dirty counts explain the existing `pager-read` dirty context totals for
  the benchmark.
- Existing storage behavior and compatibility tests pass.

## Verification Results

The requested verification completed on the VPS:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`:
  passed in `374.40 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` is `33,970,026` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `435.26 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; prepared insert step measured `71.154 us/op`.

The prepared-insert benchmark reports these non-zero pager-read site rows:

- `write_maintained_index_root_inserts / index-root`: `666` copies, `0`
  checksum-dirty copies.
- `write_maintained_index_root_overflow_flags / index-root`: `2` copies, `0`
  checksum-dirty copies.
- `redistribute_branch_index_leaf_range_entry / index-leaf`: `3,000` copies,
  `3,000` checksum-dirty copies.
- `redistribute_branch_index_leaf_range_entry / index-branch`: `7,303`
  copies, `4,221` checksum-dirty copies.
- `split_branch_index_leaf_entry / index-leaf`: `75` copies, `75`
  checksum-dirty copies.
- `split_branch_index_leaf_entry / index-branch`: `384` copies, `243`
  checksum-dirty copies.

The dirty site counts sum to the existing `pager-read` dirty context totals:
`3,075` index-leaf dirty copies and `4,464` index-branch dirty copies.

## Risks

The caller-function names may identify a broad helper rather than a precise
inner branch operation. If that happens, a follow-up slice can tag sub-sites
inside the broad helper rather than changing storage behavior from insufficient
evidence.
