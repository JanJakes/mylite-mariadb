# Dirty Checksum Refresh Counters

## Problem

After catalog-image checksum elision, the prepared-insert benchmark still
reports `284,426` zero-tail checksum calls. The page-family table identifies
the largest families:

- `index-leaf`: `121,256`
- `row`: `120,729`
- `index-branch`: `41,340`

Those counters do not show whether calls are fresh encodes or deferred
dirty-buffer refreshes. The next checksum-lifecycle change needs that split so
it does not move work between step and commit without evidence.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- Existing checksum profile counters live in
  `packages/mylite-storage/src/storage.c::checksum_page()` and
  `checksum_page_zero_tail()`.
- Dirty append and dirty page buffers converge on
  `refresh_dirty_buffered_page_checksum()` before generic reads or flushes.
- The prepared-insert benchmark prints storage test-hook counters in
  `tools/mylite_perf_baseline.c::print_prepared_insert_storage_counters()`.

## Design

Add a test-hook dirty-refresh counter beside the existing checksum family
counters:

- keep the existing full-page and zero-tail page-family counts;
- count calls that enter `refresh_dirty_buffered_page_checksum()` by page
  family after the page shape is accepted for refresh;
- reset the dirty-refresh family counters with prepared-insert profile
  counters; and
- print a third `Dirty refresh` column in the prepared-insert checksum
  page-family table.

This slice only instruments existing refresh paths. It does not change when
dirty pages are refreshed, copied, flushed, or checksummed.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Prepared inserts still route through the
same MyLite storage engine for `ENGINE=InnoDB`.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file behavior
changes. Dirty pages still refresh checksums before durable publication or
generic checksum-validating reads.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is
limited to test-hook symbols and benchmark reporting. No dependency or license
change.

## Test And Verification Plan

- Add focused storage test-hook coverage proving dirty row-page checksum
  refreshes increment the dirty-refresh family counter.
- Run the prepared-insert benchmark and record the dirty-refresh split.
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

- Prepared-insert checksum page-family output includes a dirty-refresh column.
- Dirty row-page refreshes increment the dirty-refresh family counter.
- Existing aggregate checksum counters remain available.
- Existing storage and embedded storage-engine tests pass.

## Verification

Verified on the VPS worktree on 2026-05-28:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed; clang-format reported no modified files.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `1/1` test in `344.37 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; produced `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  at `32.40 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `2/2` tests in `346.10 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step measured `76.518 us/op`. The dirty-refresh
  split showed `95,684` of `121,256` index-leaf zero-tail calls and `33,415`
  of `41,340` index-branch zero-tail calls came through dirty refreshes, while
  row zero-tail calls were mostly fresh encodes (`6,643` dirty refreshes out
  of `120,729`).

## Risks

The counter measures refresh calls, not exclusive CPU cost. It is evidence for
selecting the next checksum slice, not a replacement for correctness tests when
checksum lifecycle changes are made.
