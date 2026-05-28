# Dirty Refresh Source Family Counters

## Problem

Prepared-insert checksum output now reports dirty-refresh totals by source and
by page family, but not by the combination of both dimensions. The latest run
shows:

- `dirty-page-flush`: `54,433` refreshes.
- `append-buffer-flush`: `6,849` refreshes.
- `dirty-page-copy`: `7,539` refreshes.
- page-family dirty refreshes include `57,507` index-leaf, `4,465`
  index-branch, `6,643` row, and `210` index-entry refreshes.

The next insert-path slice needs to know whether non-flush work such as
`dirty-page-copy` is mostly index-leaf, branch, row, or index-entry work, and
whether that work is in the timed insert loop or deferred commit.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `refresh_dirty_buffered_page_checksum()` records dirty refreshes through
  `test_record_dirty_checksum_refresh()` after it validates the target page
  shape.
- Existing test-hook state already tracks dirty refreshes by page family and by
  source separately.
- `tools/mylite_perf_baseline.c` already snapshots dirty-refresh source totals
  and page-family totals before commit, after commit, and after verification.

## Design

Add a test-hook source/family dirty-refresh matrix:

- increment `(source, family)` alongside the existing source and family totals;
- reset the matrix with prepared-insert profile counters;
- expose a getter for `(source, family)` counts;
- print an aggregate benchmark table with page families as rows and refresh
  sources as columns; and
- snapshot the matrix so the benchmark can print non-zero source/family rows
  split across insert loop, commit, and verification phases.

This slice is measurement-only. It does not change checksum computation,
dirty-buffer publication, page layout, recovery, or SQL behavior.

## Affected Subsystems

- MyLite storage test-hook counters.
- Prepared-insert performance benchmark output.
- Storage test-hook counter coverage.

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

- Extend dirty checksum refresh counter coverage to assert a known row
  `test-hook` refresh increments the `(test-hook, row)` source/family cell and
  rejects out-of-range source/family lookups.
- Run the prepared-insert benchmark and record the non-zero source/family phase
  rows.
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

- Dirty-refresh source/family matrix counters reset, increment, and expose
  bounds-checked values through test hooks.
- Prepared-insert benchmark output reports aggregate source/family refresh
  counts.
- Prepared-insert benchmark output reports non-zero source/family phase deltas
  for insert loop, commit, and verification.
- Existing storage and embedded storage-engine tests pass.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `287.45 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed, `32.40 MiB` archive.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `342.47 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step was `79.409 us/op`, commit was `48.094 ms`.

The new source/family phase table showed the timed insert-loop dirty refresh
shape:

- `dirty-page-flush` / `index-leaf`: `54,432`;
- `append-buffer-flush` / `row`: `4,076`;
- `append-buffer-copy` / `index-entry`: `4`;
- `dirty-page-copy` / `index-leaf`: `3,075`;
- `dirty-page-copy` / `index-branch`: `4,464`.

Commit-phase dirty refreshes were:

- `dirty-page-flush` / `index-branch`: `1`;
- `append-buffer-flush` / `row`: `2,567`;
- `append-buffer-flush` / `index-entry`: `206`.

## Risks

The added benchmark output is diagnostic and should stay scoped to
`MYLITE_STORAGE_TEST_HOOKS`. If the matrix becomes too verbose, keep only
non-zero phase rows in the phase table.
