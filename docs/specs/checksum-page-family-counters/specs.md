# Checksum Page Family Counters

## Problem

The current prepared-insert benchmark on the VPS reports `384,427`
zero-tail checksum calls and `37,460` full-page checksum calls, while branch
leaf-range plan reads, packed-tail scans, and raw ordering probes are already
near zero. The aggregate checksum counters prove the remaining pressure is
checksum-shaped, but they do not identify which page families still drive it.

The next checksum optimization should be selected from page-family evidence
instead of a broad dirty-checksum change that risks active-cache and
dirty-buffer coherency.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- First-party benchmark output lives in
  `tools/mylite_perf_baseline.c::print_prepared_insert_storage_counters()`.
- First-party checksum accounting lives in
  `packages/mylite-storage/src/storage.c::checksum_page()` and
  `checksum_page_zero_tail()`.
- MyLite page families are distinguishable by the page-type field at byte `8`
  for catalog, blob, row, autoincrement, row-state, index-entry, index leaf,
  index root, index branch, free-list, and journal pages. Header pages use the
  header checksum offset without a page-type field.

## Design

Extend the existing test-hook prepared-insert counters with per-page-family
checksum accounting:

- classify counted checksum calls by page type plus checksum offset;
- keep the existing aggregate full-page and zero-tail checksum counters;
- expose a stable list of page-family counter names and full-page/zero-tail
  getter hooks for the benchmark;
- reset family counters with the prepared-insert profile counters; and
- print a compact prepared-insert checksum table with full-page and zero-tail
  columns.

This is an instrumentation slice only. It does not change page bytes,
checksum algorithms, durable publication, active caches, or dirty-buffer
ownership.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Prepared inserts still route through the
same MyLite handler for `ENGINE=InnoDB`.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file behavior
changes. The counters are process-local transient test-hook state.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is
limited to test-hook symbols and benchmark reporting. No dependency or license
change.

## Test And Verification Plan

- Add focused storage test-hook coverage for classifying header, index leaf,
  index branch, and unknown checksum calls.
- Run the prepared-insert benchmark and confirm it prints the page-family
  checksum table.
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

- Prepared-insert benchmark output identifies full-page and zero-tail checksum
  calls by page family.
- Existing aggregate checksum counters remain available.
- Storage test hooks prove representative page-family classification.
- Existing storage and embedded storage-engine tests pass.

## Verification

Verified on the VPS worktree on 2026-05-28:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed; clang-format reported no modified files.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `1/1` test in `315.62 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; produced `build/mariadb-mylite-storage-smoke/libmysqld/libmariadbd.a`
  at `32.40 MiB`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `2/2` tests in `362.08 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step measured `83.564 us/op`. The page-family table
  reported zero-tail checksum calls concentrated in `index-leaf` (`121,256`),
  `row` (`120,729`), `catalog` (`100,001`), and `index-branch` (`41,340`).
  Full-page checksum calls were led by `index-leaf` (`25,182`),
  `index-branch` (`8,309`), and `index-root` (`2,803`).

## Risks

The family classifier is diagnostic-only. A malformed page may fall into the
`unknown` bucket, which is acceptable for profile evidence and safer than
assuming page identity from a checksum offset alone.
