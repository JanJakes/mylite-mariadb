# Prepared Insert Profile Counters

## Problem

After the branch max-child and leaf-range identity-order slices, prepared-insert
sampling on the VPS still shows storage CPU under branch insert maintenance, but
the visible remaining hot spots have shifted toward checksum generation and
small residual ordering probes. The performance baseline currently reports
branch planning, tail-overlay, and packed-tail scan counters, but it does not
report checksum calls or raw entry ordering counts.

The next checksum-oriented slice needs direct run evidence from
`mylite_perf_baseline --phase=prepared-insert-components`, not only sparse
profiler samples.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- First-party prepared-insert performance output lives in
  `tools/mylite_perf_baseline.c::print_prepared_insert_storage_counters()`.
- Storage test hooks in `packages/mylite-storage/src/storage.c` already count
  raw entry order builds and probes for focused storage tests.
- `checksum_page()` already has a test-only call counter gate, but
  `checksum_page_zero_tail()` does not yet contribute a separate count even
  though profiles show it as the dominant checksum routine.

## Design

Add test-hook counters for prepared-insert profiling:

- count `checksum_page_zero_tail()` calls behind the existing checksum-count
  gate;
- expose reset/getter hooks for full-page checksum calls, zero-tail checksum
  calls, raw entry order builds, and raw entry order probes;
- have the prepared-insert benchmark reset those counters before the measured
  phase; and
- print the counts in the existing prepared-insert storage counter table.

The counters are diagnostic-only test hooks. They do not alter storage
semantics, page bytes, or benchmark timing phases beyond the existing test-hook
instrumentation cost.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. The counters are compiled only for the
test/performance harness configuration that already uses storage test hooks.

## Single-File And Lifecycle Impact

No durable file-format, journal, recovery, lock, or companion-file change.
Counters are process-local transient state.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is
limited to test-hook symbols and benchmark reporting. No dependency change.

## Test And Verification Plan

- Run the prepared-insert benchmark and confirm it reports checksum and raw
  order counters.
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

- Prepared-insert benchmark output reports full-page checksum calls,
  zero-tail checksum calls, raw entry order builds, and raw entry order probes.
- Existing storage and embedded storage-engine tests pass.
- No durable storage or SQL behavior changes.

## Verification

Run on the VPS worktree on 2026-05-28:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed; clang-format reported no modified files.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  1/1 tests in 322.02 seconds.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; rebuilt `libmysqld/libmariadbd.a` at 32.40 MiB with
  `PLUGIN_MYLITE_SE=STATIC`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, 2/2 tests in 382.49 seconds.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared insert step measured 82.235 us/op. The prepared-insert storage
  counter table reported 37,460 full-page checksum calls, 384,427 zero-tail
  checksum calls, 7,918 raw entry order builds, and 8,032,627 raw entry order
  probes.

## Risks

Counting checksum calls can add a tiny test-hook branch in benchmark builds.
That is acceptable for local profiling because the counters are diagnostic and
do not change release storage semantics.
