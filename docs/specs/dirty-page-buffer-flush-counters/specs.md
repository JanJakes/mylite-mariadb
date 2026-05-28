# Dirty Page Buffer Flush Counters

## Problem

After the full-window dirty-page buffer change, prepared-insert counters still
show insert-loop dirty-page flush refreshes:

- dirty-page flush refreshes: `79,828`
- insert-loop dirty refreshes: `91,016`
- insert-loop zero-tail checksum calls: `132,411`

The source table identifies refreshes performed by dirty-page flush, but it
does not say why the dirty-page buffer flushed or how full each flush was. The
next optimization needs to distinguish buffer-limit flushes from root statement
commit flushes before changing buffer lifetime again.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- Root statement commit flushes dirty pages from
  `mylite_storage_commit_statement()`.
- Buffer-limit pressure flushes dirty pages from
  `store_dirty_page_in_buffer()` before the next page is appended.
- Tests call `flush_statement_dirty_page_buffer()` directly for dirty-buffer
  checksum lifecycle coverage.
- The prepared-insert benchmark already prints test-hook storage counters.

## Design

Add test-hook counters around dirty-page buffer flushes:

- pass a small flush-source enum into `flush_statement_dirty_page_buffer()`;
- count flush calls and pages flushed by source when prepared-insert profile
  counting is enabled;
- reset the flush counters with prepared-insert profile counters;
- expose source names, flush counts, and page counts to the benchmark; and
- print a dirty-page buffer flush source table in prepared-insert benchmark
  output.

The source buckets are:

- `buffer-limit`
- `statement-commit`
- `test-hook`

This slice does not change when dirty pages flush or how pages are written.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Prepared inserts still route through the
same MyLite storage engine for `ENGINE=InnoDB`.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file behavior
changes. The counters are transient test-hook state.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is
limited to test-hook symbols and benchmark reporting. No dependency or license
change.

## Test And Verification Plan

- Extend dirty-buffer checksum tests to assert direct test-hook flush call and
  page counters.
- Run the prepared-insert benchmark and record flush source counts and pages.
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

- Prepared-insert benchmark output identifies dirty-page buffer flush calls and
  pages by source.
- Focused test-hook coverage proves direct flush counters increment.
- Existing storage and embedded storage-engine tests pass.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `281.04 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed, `32.40 MiB` archive.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `349.18 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step was `79.611 us/op`, commit was `43.550 ms`.
  Dirty-page buffer flush source evidence:
  - `buffer-limit`: `5,009` flushes, `80,144` pages.
  - `statement-commit`: `1` flush, `1` page.
  - `test-hook`: `0` flushes, `0` pages.

The same benchmark run reported `79,828` dirty-page-flush checksum refreshes
and `6,849` append-buffer-flush refreshes. Phase counters kept the hot-loop
work separated from commit and verification: insert loop `37,458` full-page,
`132,411` zero-tail, and `91,016` dirty refresh calls; commit `0` full-page,
`2,774` zero-tail, and `2,773` dirty refresh calls; verification `2`
full-page, `107,078` zero-tail, and `0` dirty refresh calls.

## Risks

The counters measure flush shape, not exclusive CPU cost. They are evidence for
choosing a later buffer-lifetime optimization.
