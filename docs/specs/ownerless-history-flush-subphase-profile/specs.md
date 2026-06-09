# Ownerless History Flush Subphase Profile

## Problem

The ownerless autocommit performance audit now uses CI production artifacts:
first-party MyLite builds are `Release`, and the MariaDB embedded archive is
the documented `MinSizeRel` baseline. Those production probes show the largest
remaining ownerless autocommit cost inside the native InnoDB write-history
proof, but the existing deep counter only reports the whole ownerless
rollback-segment-space flush interval.

Before changing durability-sensitive flush behavior, MyLite needs to know
whether that cost is the exact-page lookup/flush attempt, the synchronous AIO
wait for those page writes, the space-wide fallback, or a redo-log flush after
the pages are clean.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` times the whole
  `mylite_ownerless_innodb_flush_history_pages_to_lsn()` call as
  `...OWNERLESS_FLUSH_NS` and records exact flushed pages and fallback rounds.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  `mylite_ownerless_innodb_flush_history_pages_to_lsn()` passes the
  rollback-segment header page and undo header page into
  `buf_flush_wait_space_pages_flushed()`.
- `mariadb/storage/innobase/buf/buf0flu.cc`
  `buf_flush_wait_space_pages_flushed()` checks whether the rollback-segment
  tablespace has old dirty pages, tries the known page numbers with
  `buf_flush_try_space_pages()`, waits for the exact writes with
  `os_aio_wait_until_no_pending_writes(false)`, rechecks the tablespace, falls
  back to `buf_flush_wait_space_flushed_slow()` when needed, then calls
  `log_write_up_to(sync_lsn, true)` if the redo log has not reached the target.

## Design

Keep the exact flush and fallback algorithm unchanged. Add tail counters to
the existing MyLite deep InnoDB performance enum for:

- dirty-page needs checks,
- exact known-page flush attempt,
- exact-write AIO wait,
- space-wide fallback,
- final redo-log write.

Record those timings only through the existing
`mylite_ownerless_innodb_deep_perf_start_ns()` and
`mylite_ownerless_innodb_deep_perf_add_elapsed()` helpers. When deep stats are
disabled, the start helper returns zero and the elapsed helper does not read
the clock or update counters, so the stats-off throughput probe remains the
production performance signal.

Expose the new counters in `mylite_embedded_performance_probe` both as detailed
`mylite_perf_ownerless_insert_autocommit_innodb_deep_*` lines and as compact
`mylite_perf_summary_ownerless_autocommit_*_ms_per_insert` summary keys.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, metadata, wire-protocol, or storage-format
behavior changes. The slice changes diagnostic counters emitted by performance
tests only.

## Directory And Lifecycle Impact

No durable directory-layout changes. The same native InnoDB pages are flushed
inside the MyLite database directory, and the existing space-wide fallback
remains the correctness proof if exact page flushing is incomplete.

## Native Storage Impact

No native storage behavior changes. The counters attribute the existing native
history flush boundary; they do not skip page writes, redo writes, doublewrite
buffer flushing, AIO waits, or fallback checks.

## Build And Performance Impact

Stats-off probes still avoid these timing calls. Stats-enabled attribution runs
perform a few extra clock reads around existing flush subphases. CI timings
remain production-build based:

- normal matrix jobs use the `prod` preset,
- embedded CTest, ownerless SQL, and embedded performance probes use
  `php-embedded-prod`,
- the MariaDB embedded archive uses `MinSizeRel`,
- WordPress timing phases use `build/wordpress-php-embedded-prod` with
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release` and
  `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1`.

## Test And Verification Plan

- Verify local production CMake caches with `tools/require-cmake-release-build`
  and `tools/require-cmake-build-type MinSizeRel`.
- Build the production embedded performance probe and ownerless SQL test.
- Run the focused native history proof selector.
- Run a reduced production stats-enabled embedded attribution probe and confirm
  the new exact-flush subphase summary keys are present.
- Run focused ownerless primitive coverage affected by the commit path.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-09 used production artifacts:
`build/mariadb-embedded` and `build/wordpress-mariadb-embedded` were
`MinSizeRel`, while `build/prod`, `build/php-embedded-prod`, and
`build/ownerless-test-hooks` were `Release`.

- `tools/require-cmake-release-build build/prod build/php-embedded-prod
  build/ownerless-test-hooks` passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded
  build/wordpress-mariadb-embedded` passed.
- `tools/mariadb-embedded-build build` passed and rebuilt
  `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed after the MariaDB rebuild and again after formatting.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-single-owner-history-flush-native-proof$'
  --output-on-failure` passed before and after formatting.
- A focused production ownerless CTest set passed:
  `ownerless-primitives`, `ownerless-single-owner-page-write-refresh-skip`,
  `ownerless-single-owner-external-refresh-skip`,
  `ownerless-single-owner-history-flush-native-proof`, and
  `ownerless-uncommitted-peer-hidden`.
- A reduced production stats-enabled attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=50`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` printed the new summary keys.
  The post-format sample reported `2.000` exact history flush pages per
  insert, `0.000` fallback rounds per insert, `0.227 ms/insert` in exact page
  try time, `0.736 ms/insert` in exact-write AIO wait time,
  `0.000 ms/insert` in fallback time, and `0.004 ms/insert` in final redo-log
  write time. Ownerless autocommit throughput in that stats-enabled sample was
  `246.65 ops/s`.
- A stats-off production throughput sample with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000` reported ownerless transactional
  inserts at `1206.40 ops/s` and ownerless autocommit inserts at
  `446.88 ops/s`, within the previously observed noisy branch range.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed, and
  `tools/require-cmake-release-build build/ownerless-test-hooks` accepted the
  hook cache.
- Hook selectors `visible-publish-crash` and `visible-checkpoint-crash` passed
  through the hook-built ownerless SQL test binary.
- `cmake --build --preset format` passed.
- `cmake --build --preset format-check-prod` passed after formatting.
- `git diff --check` passed before and after formatting.

## Acceptance Criteria

- Existing ownerless history flush behavior and fallback semantics are
  unchanged.
- Deep stats expose exact flush needs-check, try, AIO wait, fallback, and
  redo-log-write timings.
- The embedded attribution probe prints compact per-insert summaries for those
  timings.
- Focused correctness and production attribution checks pass.

## Risks And Unresolved Questions

- This slice identifies the expensive subphase; it does not by itself improve
  ownerless autocommit throughput.
- Reduced local production samples remain noisy. Optimization decisions still
  need repeated stats-off samples under comparable load and storage placement.
- Broader native redo/checkpoint reconciliation remains the larger correctness
  prerequisite before reducing the native proof itself.
