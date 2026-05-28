# Dirty Page Pressure Write Site Counters

## Problem

After deferring dirty-page undo checksum refreshes, the prepared-insert
benchmark reports `0` hot-path `dirty-page-copy` checksum refreshes. The next
dominant storage work is dirty-page buffer pressure:

- buffer-limit flushes: `54,432` one-page flushes;
- buffer-limit flush pages: all `index-leaf`;
- pressure incoming pages: mostly checksum-dirty `index-leaf` pages.

The pressure counters identify victim and incoming page families, but not the
writer path that admits the page forcing pressure. Before changing eviction or
buffering policy again, the benchmark needs function-level pressure evidence.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Prepared inserts reach maintained-index writes through MyLite storage in
  `packages/mylite-storage/src/storage.c`.
- `pager_write_maintained_insert_page()` is the common maintained-index write
  entry for root, branch, and leaf insert maintenance.
- When maintained existing-page writes are bufferable,
  `pager_write_buffered_maintained_index_page()` calls
  `buffer_dirty_page_for_pager_write()`, which eventually calls
  `store_dirty_page_in_buffer()`.
- `store_dirty_page_in_buffer()` records pressure incoming page-family
  counters after a buffer-limit pressure flush succeeds, but it has no caller
  attribution.

## Design

Add test-hook pressure write-site counters:

- Thread-locally record the caller function while
  `pager_write_maintained_insert_page()` executes.
- Carry that borrowed write-site name on test-hook dirty-buffer entries, so
  pressure that occurs while merging nested statement dirty pages into a parent
  statement can still be attributed to the original maintained writer.
- Keep existing dirty-page buffer pressure, flush, and replacement counters
  unchanged.
- When a buffer-limit pressure admission records the incoming page family, also
  record total and checksum-dirty incoming counts by maintained write site and
  page family.
- Use a fixed-size test-hook site table with borrowed `__func__` strings; do
  not allocate memory or change runtime storage structures.
- Print a compact non-zero pressure write-site/family table in
  `mylite_perf_baseline`.

This slice is observability-only. It does not change dirty-page buffer size,
eviction selection, write ordering, rollback, or journal behavior.

## Affected Subsystems

- MyLite storage test hooks.
- Prepared-insert performance benchmark output.
- Dirty-page buffer pressure observability.

No MariaDB SQL-layer or handler-layer code changes are planned.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. The slice only records test-hook counters
around existing maintained-index writes.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Journal, rollback, dirty
buffer, active statement, and pager lifetimes are unchanged.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API, durable file-format, dependency, or license change.
Binary-size impact is limited to test-hook builds and the development
benchmark tool. The fixed site table avoids per-write allocations.

## Test And Verification Plan

- Add storage test-hook coverage proving a maintained-index write that admits a
  pressure page records the caller function name, page family, and
  checksum-dirty state.
- Run the prepared-insert benchmark and compare write-site/family pressure
  counts with the existing pressure incoming totals.
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

- Buffer-limit pressure admissions record maintained write site, page-family,
  and checksum-dirty counts in test-hook builds.
- The prepared-insert benchmark prints non-zero pressure write-site/family
  rows.
- Pressure write-site counts explain the benchmark's pressure incoming family
  totals.
- Existing storage behavior and compatibility tests pass.

## Risks

The first pass attributes only maintained-index writes routed through
`pager_write_maintained_insert_page()`. Direct `store_dirty_page_in_buffer()`
test helpers and any future non-maintained dirty-page producers will show no
site until they get their own site tagging.

## Verification Results

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c` passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed
  in `340.77 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC` passed; archive size remained
  `33,968,682` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test` passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure` passed in
  `370.91 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000` passed. Prepared insert step measured
  `77.417 us/op`; pressure incoming totals were `54,289` dirty `index-leaf`
  pages and `143` `index-branch` pages, `105` dirty. Write-site attribution
  matched those totals: `insert_branch_index_leaf_entry` admitted `54,289`
  dirty `index-leaf` pages and `105` dirty `index-branch` pages, and
  `redistribute_branch_index_leaf_range_entry` admitted `38` clean
  `index-branch` pages.
