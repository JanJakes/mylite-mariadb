# Dirty Page Buffer Flush Family Counters

## Problem

After one-page dirty-buffer pressure eviction, prepared inserts still report
substantial dirty-page flush work:

- dirty-page-flush checksum refreshes: `61,049`;
- insert-loop dirty refreshes: `72,595`;
- buffer-limit pressure: `61,287` one-page flushes.

The benchmark identifies the flush trigger and total pages, but it does not
show which page families are being published under buffer-limit pressure. The
next eviction-policy change needs to know whether pressure mostly flushes
index leaves, branch ancestors, roots, or another page family.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- MyLite dirty-page buffer publication happens through
  `flush_statement_dirty_page_buffer()` for root statement commit/test hooks
  and through `flush_dirty_page_buffer_entry()` for buffer-limit pressure.
- Test-hook checksum page-family classification already lives in
  `test_checksum_page_family()` and backs prepared-insert checksum family
  output.
- The prepared-insert benchmark already prints dirty-page buffer flush counts
  by source.

## Design

Add test-hook counters for dirty-page buffer flush pages by source and page
family:

- classify each dirty-page buffer entry at flush time using the existing
  test-hook page-family vocabulary;
- keep source-level flush call/page counters unchanged;
- count pages by source and family for both whole-buffer flushes and one-page
  pressure evictions;
- reset the family counters with prepared-insert profile counters;
- expose a getter for `(source, family)` page counts; and
- print a benchmark table with page families as rows and flush sources as
  columns.

This slice is measurement-only. It does not change eviction, checksum,
journal, rollback, or write behavior.

## Affected Subsystems

- MyLite storage test-hook counters.
- Prepared-insert performance benchmark output.
- Storage test-hook coverage for dirty-page buffer flush accounting.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Prepared inserts still route `ENGINE=InnoDB`
through MyLite storage.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file behavior
changes. The counters are transient test-hook state.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is
limited to test-hook counters and benchmark reporting. No dependency or license
change.

## Test And Verification Plan

- Extend dirty-buffer checksum tests to assert a test-hook index-leaf flush is
  counted under the index-leaf family.
- Run the prepared-insert benchmark and record buffer-limit flush page families.
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

- Prepared-insert benchmark output identifies dirty-page buffer flush pages by
  source and page family.
- Focused test-hook coverage proves a known index-leaf flush increments the
  source/family page counter.
- Existing storage and embedded storage-engine tests pass.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `325.46 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed, `32.40 MiB` archive.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `355.68 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step was `65.844 us/op`, commit was `47.865 ms`.

The benchmark family table showed the buffer-limit pressure shape:

- `index-leaf`: `54,341` buffer-limit pages.
- `index-branch`: `6,946` buffer-limit pages and `1` statement-commit page.
- all other families: `0` dirty-page buffer flush pages.

The unchanged source-level counters for the same run were `61,287`
buffer-limit flushes / pages, `1` statement-commit flush / page, and `61,049`
dirty-page-flush checksum refreshes.

## Risks

The counters describe page publication shape, not exclusive CPU cost. They are
intended to guide a later eviction-policy or buffer-lifetime change.
