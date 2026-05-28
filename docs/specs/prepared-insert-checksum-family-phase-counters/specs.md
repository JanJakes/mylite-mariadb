# Prepared Insert Checksum Family Phase Counters

## Problem

After dirty-page buffer pressure work, prepared-insert counters still show high
checksum volume:

- total zero-tail checksum calls: `217,299`;
- insert-loop zero-tail checksum calls: `107,446`;
- verification zero-tail checksum calls: `107,078`;
- dirty-page-flush checksum refreshes: `54,433`.

The benchmark reports page-family totals and phase totals separately, but not
page-family deltas by phase. The next checksum optimization needs to distinguish
timed insert-loop work from commit and final verification noise for row,
index-leaf, and index-branch pages.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `tools/mylite_perf_baseline.c` already snapshots prepared-insert counters
  before commit, after commit, and after verification.
- Storage test hooks already expose checksum page-family counters for
  full-page, zero-tail, and dirty-refresh calls.

## Design

Extend the prepared-insert benchmark snapshot with page-family arrays:

- record full-page, zero-tail, and dirty-refresh page-family counters in each
  phase snapshot;
- keep existing aggregate phase and dirty-refresh source tables unchanged;
- print a page-family phase table showing insert-loop, commit, and verification
  deltas for each family; and
- keep all changes benchmark/test-hook only.

The output is evidence for later storage changes. It does not change storage
behavior, file format, transaction semantics, or SQL behavior.

## Affected Subsystems

- Prepared-insert benchmark reporting.
- Documentation of storage performance evidence.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Prepared inserts still route `ENGINE=InnoDB`
through MyLite storage.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file behavior
changes.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is
limited to benchmark/test-hook reporting code. No dependency or license change.

## Test And Verification Plan

- Run the prepared-insert benchmark and record the new family-by-phase table.
- Keep storage and routed embedded storage-engine tests passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Prepared-insert benchmark output identifies checksum full-page, zero-tail,
  and dirty-refresh deltas by page family for insert loop, commit, and
  verification phases.
- Existing storage and embedded storage-engine tests pass.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed,
  `ninja: no work to do`.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `310.92 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed, `32.40 MiB` archive.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `329.76 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step was `63.075 us/op`, commit was `53.787 ms`.

The new family phase table identified the timed insert-loop work:

- `index-leaf`: `25,182` full-page, `83,079` zero-tail, and `57,507` dirty
  refresh calls in the insert loop.
- `index-branch`: `8,309` full-page, `12,389` zero-tail, and `4,464` dirty
  refresh calls in the insert loop; commit added `1` zero-tail/dirty refresh.
- `row`: `11,084` zero-tail and `4,076` dirty refresh calls in the insert loop,
  `2,567` zero-tail/dirty refresh calls at commit, and `107,078` zero-tail
  calls during verification.
- `index-entry`: `224` zero-tail and `4` dirty refresh calls in the insert
  loop, then `206` zero-tail/dirty refresh calls at commit.

## Risks

This slice measures distribution, not exclusive CPU cost. It should guide the
next checksum-lifecycle optimization rather than being treated as performance
improvement by itself.
