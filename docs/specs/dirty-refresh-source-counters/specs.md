# Dirty Refresh Source Counters

## Problem

The prepared-insert checksum page-family counters now show that most remaining
index-leaf and index-branch zero-tail checksum calls come from deferred
dirty-buffer refreshes:

- `index-leaf`: `95,684` dirty refreshes out of `121,256` zero-tail calls.
- `index-branch`: `33,415` dirty refreshes out of `41,340` zero-tail calls.
- `row`: `6,643` dirty refreshes out of `120,729` zero-tail calls.

Those counters still do not identify which lifecycle boundary performs the
refresh. The next optimization needs to know whether work is dominated by
copy-for-read, append-buffer flush, dirty-page flush, or direct maintained-page
writes before changing checksum timing.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- First-party dirty checksum refreshes converge on
  `packages/mylite-storage/src/storage.c::refresh_dirty_buffered_page_checksum()`.
- Current refresh call sites include dirty-page buffer flush,
  append-page buffer flush, maintained-page direct writes, append-buffer
  copy-for-read, dirty-page copy-for-read, and storage test hooks.
- The prepared-insert benchmark prints storage test-hook counters in
  `tools/mylite_perf_baseline.c::print_prepared_insert_storage_counters()`.

## Design

Add a test-hook source counter beside the existing dirty-refresh page-family
counter:

- pass a small internal source enum into each
  `refresh_dirty_buffered_page_checksum()` call site;
- count accepted dirty refreshes by source after the page shape is validated;
- reset source counters with prepared-insert profile counters;
- expose source slot count, source name, and count getters for the benchmark;
  and
- print a compact prepared-insert dirty-refresh source table.

The source buckets are diagnostic labels only:

- `dirty-page-flush`
- `append-buffer-flush`
- `maintained-direct-write`
- `append-buffer-copy`
- `dirty-page-copy`
- `test-hook`

This slice does not change when dirty pages are refreshed, copied, flushed, or
checksummed.

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

- Add focused storage test-hook coverage proving source counters increment for
  a test-hook refresh and for dirty-page copy and flush refreshes.
- Run the prepared-insert benchmark and record the source split.
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

- Prepared-insert benchmark output identifies dirty checksum refreshes by
  lifecycle source.
- Existing aggregate and page-family checksum counters remain available.
- Storage test hooks prove representative source-counter increments.
- Existing storage and embedded storage-engine tests pass.

## Verification

Verified on the VPS worktree on 2026-05-28:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed; clang-format reported no modified files.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `1/1` test in `311.85 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; produced `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  at `32.40 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `2/2` tests in `341.20 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step measured `75.706 us/op`. The dirty-refresh
  source table reported `121,271` dirty-page flush refreshes, `7,828`
  dirty-page copy refreshes, `6,849` append-buffer flush refreshes, `4`
  append-buffer copy refreshes, `0` maintained-direct-write refreshes, and `0`
  test-hook refreshes.

## Risks

The source counter measures refresh calls, not exclusive CPU cost. It is
evidence for selecting the next checksum-lifecycle slice, not a replacement for
correctness tests when checksum timing changes.
