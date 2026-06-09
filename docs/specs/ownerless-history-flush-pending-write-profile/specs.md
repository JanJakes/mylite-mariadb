# Ownerless History Flush Pending Write Profile

## Problem

The ownerless exact AIO wait profile showed the reduced autocommit workload is
waiting in InnoDB's write-slot drain, not in doublewrite-buffer completion. That
still leaves one important ambiguity: the global `write_slots->wait()` call may
be waiting only for the exact rollback-segment and undo pages that MyLite just
scheduled, or it may be draining unrelated pending native writes.

MyLite needs this distinction before choosing between a narrow target-page wait
optimization and broader native redo/checkpoint reconciliation.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/buf/buf0flu.cc`
  `buf_flush_wait_space_pages_flushed()` calls
  `buf_flush_try_space_pages()` for the known rollback-segment and undo page
  numbers, then waits for writes through the ownerless-profiled AIO wrapper.
- `mariadb/storage/innobase/os/os0file.cc`
  `os_aio_wait_until_no_pending_writes_low()` drains all asynchronous write
  slots through `write_slots->wait()`.
- The same file already reads `write_slots->pending_io_count()` before entering
  the low wait when it needs to decide whether to declare a thread-pool wait.
  The count is therefore available at the same layer as the ownerless AIO child
  wait instrumentation.

## Design

Keep the exact native history flush algorithm unchanged. Extend the
ownerless-profiled AIO wrapper to add two counters:

- pending write-slot count before the low wait,
- pending write-slot count after the low wait.

Expose those counters in the embedded attribution probe as detailed deep stats
and compact per-insert summary keys. Compare the before-wait count with the
existing exact history flush page count:

- if the counts are close, the wait is dominated by the exact target pages
  themselves;
- if the pending count is materially larger, the global wait is draining
  unrelated native writes and a narrower wait primitive may be worthwhile.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, metadata, wire-protocol, directory-layout, or
storage-format behavior changes. The slice changes production performance
diagnostics only.

## Directory And Lifecycle Impact

No durable directory-layout changes. The same native InnoDB files remain inside
the MyLite database directory, and the same exact flush, fallback check, and
redo-log write behavior remains in place.

## Native Storage Impact

No native InnoDB behavior changes. This slice does not skip page writes,
doublewrite-buffer waits, AIO waits, fallback scans, checkpoints, or redo-log
writes.

## Build And Performance Impact

Stats-off runs add only disabled-counter checks in the ownerless exact history
flush wait branch. Stats-enabled attribution probes read the existing InnoDB
pending write-slot count twice per exact wait.

CI timing remains production-build based:

- normal matrix jobs use the `prod` preset,
- embedded CTest, ownerless SQL, and embedded performance probes use
  `php-embedded-prod`,
- MariaDB embedded archives use `MinSizeRel`,
- WordPress PHP extension timing phases use
  `build/wordpress-php-embedded-prod` with
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release` and the Release guard enabled.

## Test And Verification Plan

- Verify local production CMake caches with `tools/require-cmake-release-build`
  and `tools/require-cmake-build-type MinSizeRel`.
- Rebuild the MariaDB embedded archive after editing InnoDB source.
- Build the production embedded performance probe and ownerless SQL test.
- Run the focused ownerless native history proof selector.
- Run a reduced production stats-enabled embedded attribution probe and confirm
  the pending write-slot summary keys are present.
- Run focused ownerless primitive coverage affected by the commit path.
- Run `cmake --build --preset format`.
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
  passed.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-single-owner-history-flush-native-proof$'
  --output-on-failure` passed.
- A reduced production stats-enabled attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` printed the new pending-write
  keys. The sample reported ownerless autocommit at `212.84 ops/s`, `2.000`
  exact history flush pages per insert, `0.000` fallback rounds per insert,
  `2.150 ms/insert` in exact AIO wait, `0.940` pending writes before the
  write-slot wait per insert, `2.148 ms/insert` in write-slot wait, `0.000`
  pending writes after the wait per insert, effectively zero doublewrite wait,
  `0.000 ms/insert` in fallback time, and `0.007 ms/insert` in final redo-log
  write time.
- Focused production selectors passed for `ownerless-primitives`,
  `ownerless-single-owner-page-write-refresh-skip`,
  `ownerless-single-owner-external-refresh-skip`,
  `ownerless-single-owner-history-flush-native-proof`, and
  `ownerless-uncommitted-peer-hidden`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed, and
  `tools/require-cmake-release-build build/ownerless-test-hooks` accepted the
  hook cache.
- Hook selectors `visible-publish-crash` and `visible-checkpoint-crash` passed
  through the hook-built ownerless SQL test binary.
- A stats-off production throughput sample with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000` reported ownerless transactional
  inserts at `1424.76 ops/s` and ownerless autocommit inserts at
  `525.56 ops/s`.
- `cmake --build --preset format` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Existing ownerless history flush behavior and fallback semantics are
  unchanged.
- The embedded attribution probe keeps the existing exact AIO wait and child
  wait summary keys.
- The embedded attribution probe also prints pending write-slot counts before
  and after the exact write-slot wait.
- Focused correctness and production attribution checks pass.

## Risks And Unresolved Questions

- The pending count is a queue-depth attribution signal, not a direct
  correctness proof for a narrower wait.
- If the pending count matches the exact page count, avoiding the current cost
  requires either avoiding per-autocommit native page writes or proving the same
  pages through ownerless WAL/redo reconciliation.
- If the pending count is larger than the exact page count, a future slice must
  still prove that a narrower wait observes durable target-page completion
  before the history proof is relaxed.
