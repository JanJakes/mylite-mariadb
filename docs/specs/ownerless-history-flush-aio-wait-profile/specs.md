# Ownerless History Flush AIO Wait Profile

## Problem

Production ownerless attribution now shows the exact native history flush path
does not fall back to a space-wide scan on the reduced autocommit probe. The
largest remaining subphase is the exact-write AIO wait inside
`os_aio_wait_until_no_pending_writes(false)`, but that MariaDB helper waits for
both normal asynchronous write slots and doublewrite-buffer completion.

Before attempting a durability-sensitive optimization, MyLite needs to know
which native wait is dominating the ownerless rollback-segment-space proof.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/buf/buf0flu.cc`
  `buf_flush_wait_space_pages_flushed()` flushes the known rollback-segment and
  undo header pages, then waits with
  `os_aio_wait_until_no_pending_writes(false)` before rechecking whether the
  rollback-segment tablespace is clean through the target LSN.
- `mariadb/storage/innobase/os/os0file.cc`
  `os_aio_wait_until_no_pending_writes()` calls
  `os_aio_wait_until_no_pending_writes_low()` and then
  `buf_dblwr.wait_flush_buffered_writes()`.
- The low helper drains `write_slots->wait()`, while
  `mariadb/storage/innobase/include/buf0dblwr.h`
  `buf_dblwr_t::wait_flush_buffered_writes()` waits while a doublewrite batch is
  running.
- The previous exact-flush subphase profile retained the existing total AIO
  wait key, so CI branch/main timing comparisons can keep using the stable
  total while this slice adds child attribution.

## Design

Keep the exact native history flush algorithm unchanged. Add two deep InnoDB
stats counters under the existing exact AIO wait total:

- write-slot wait time,
- doublewrite-buffer wait time.

Add a narrow `mylite_ownerless_*` wrapper in `os0file.cc` that calls the same
low wait and doublewrite wait as `os_aio_wait_until_no_pending_writes()`, but
records the two elapsed intervals through the existing
`mylite_ownerless_innodb_deep_perf_*` helpers. Use this wrapper only from the
ownerless exact history flush call site in `buf_flush_wait_space_pages_flushed()`.

When stats are disabled, the helper performs the same waits and the timing
helpers do not read the clock.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, metadata, wire-protocol, directory-layout, or
storage-format behavior changes. The slice changes production performance
diagnostics only.

## Directory And Lifecycle Impact

No durable directory-layout changes. The same native InnoDB files remain inside
the MyLite database directory, and the same exact flush, fallback check, and
redo-log write behavior remains in place.

## Native Storage Impact

No native InnoDB behavior changes. This slice does not skip data-file writes,
doublewrite-buffer waits, AIO waits, fallback scans, checkpoints, or redo-log
writes. It only attributes the existing wait.

## Build And Performance Impact

Stats-off runs keep the same native wait path and add only a couple of disabled
counter checks inside the ownerless exact history flush branch. Stats-enabled
attribution probes perform two extra clocked intervals around waits that were
already present.

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
  the new exact AIO child summary keys are present.
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
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` printed the new child wait
  keys. The sample reported ownerless autocommit at `195.50 ops/s` versus
  ordinary autocommit at `839.85 ops/s`, `2.000` exact history flush pages per
  insert, `0.000` fallback rounds per insert, `3.718 ms/insert` in exact AIO
  wait, `3.717 ms/insert` in write-slot wait, effectively zero doublewrite
  wait, `0.000 ms/insert` in fallback time, and `0.004 ms/insert` in final
  redo-log write time.
- The focused ownerless CTest set passed for `ownerless-primitives`,
  `ownerless-single-owner-page-write-refresh-skip`,
  `ownerless-single-owner-external-refresh-skip`, and
  `ownerless-single-owner-history-flush-native-proof`. The same bundled run
  timed out once in `ownerless-uncommitted-peer-hidden`, and an isolated rerun
  of that selector passed in `2.73s`.
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
  inserts at `1449.02 ops/s` and ownerless autocommit inserts at
  `403.19 ops/s`, within the recent noisy branch range.
- `cmake --build --preset format` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Existing ownerless history flush behavior and fallback semantics are
  unchanged.
- The embedded attribution probe keeps the existing exact AIO wait summary key.
- The embedded attribution probe also prints per-insert write-slot wait and
  doublewrite wait summary keys.
- Focused correctness and production attribution checks pass.

## Risks And Unresolved Questions

- This slice identifies the native wait source; it does not by itself improve
  ownerless autocommit throughput.
- Reduced local samples remain noisy and must be treated as attribution
  evidence, not absolute throughput.
- If doublewrite completion dominates, a later optimization still needs a
  correctness proof for any doublewrite mode or native checkpoint change. If
  write-slot draining dominates, a later optimization needs broader native
  redo/checkpoint reconciliation before relaxing the per-autocommit proof.
