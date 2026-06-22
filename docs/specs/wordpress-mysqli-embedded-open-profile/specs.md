# WordPress Mysqli Embedded Open Profile

## Problem

The production WordPress timing artifact now separates PHPUnit test time from
build, artifact, Docker, and perf-probe phases. Recent production CI samples
show the hot active-runtime reconnect path near `2 ms`, while a short-lived PHP
process that opens and closes MyLite still costs about `130-145 ms`. The
existing WordPress process profile only reports aggregate mysqli open and close
time, so the published artifact does not show whether the native cost is open,
runtime startup, embedded connect, system-table checks, close, or runtime
shutdown.

The C embedded performance probe already records those `mylite_open()` and
`mylite_close()` subphases. This slice carries that existing internal
attribution into the opt-in mysqli profile samples used by the WordPress
perf-probe, without changing public API or lifecycle behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` owns the internal
  `mylite_embedded_open_perf_set_enabled`,
  `mylite_embedded_open_perf_reset`, and `mylite_embedded_open_perf_read`
  symbols. The table is disabled by default and records open, start-runtime,
  connect-runtime, system-table, close, and release-runtime subphases.
- `packages/libmylite/tests/embedded_performance_probe.c` already consumes the
  same table for ordinary and ownerless open/close probes.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` enables
  `mylite_exec_result_perf_*` only under `MYLITE_MYSQLI_PROFILE=1` and prints
  `mylite_mysqli_profile_*` rows during module shutdown.
- `tools/wordpress-phpunit-mysqli-mylite` runs one explicit-close and one
  implicit-object-free profiled PHP process when
  `MYLITE_WORDPRESS_PERF_PROFILE_CONNECT_PROCESSES=1`, then appends selected
  profile rows into the WordPress timing summary.

## Scope And Non-Goals

In scope:

- Enable and reset the existing embedded open-phase table when
  `MYLITE_MYSQLI_PROFILE=1`.
- Print selected `mylite_mysqli_profile_embedded_open_*` counters and average
  milliseconds from the PHP mysqli profile.
- Copy those selected profile rows from the WordPress explicit and implicit
  process profile samples into the timing summary.
- Preserve the selected rows in the final WordPress timing rollup.

Out of scope:

- Changing `mylite_open()`, `mylite_close()`, MariaDB startup, InnoDB recovery,
  PHP object cleanup, or ownerless concurrency semantics.
- Adding hard timing thresholds.
- Mirroring the much larger MariaDB startup/shutdown enum into the PHP
  extension. The embedded open table already exposes the handoff points
  (`start_mysql_server_init` and `release_mysql_shutdown`) needed to correlate
  WordPress process cost with the deeper embedded probe.

## Design

The mysqli extension reuses the existing internal symbols:

- `mylite_embedded_open_perf_reset()`;
- `mylite_embedded_open_perf_set_enabled(int)`;
- `mylite_embedded_open_perf_read(uint64_t *, size_t)`.

Those diagnostic symbols are exported so the dynamically loaded PHP extension
can resolve them, but they remain absent from `mylite.h` and are not part of
the supported public C API.

Module initialization resets the table and enables it only when
`MYLITE_MYSQLI_PROFILE=1`. Module shutdown disables and reads the table before
printing the normal profile block. The profile output prints a bounded subset
of process-open attribution keys:

- open calls and open total/start/connect/system-table averages;
- start-runtime calls, total average, and `mysql_server_init()` average;
- embedded connect calls and `mysql_real_connect()` average;
- close calls and close total/release-runtime averages;
- release-runtime calls, total average, and combined MariaDB shutdown average.

The WordPress perf-probe profile copier already has stable explicit and
implicit process shapes. It adds the new keys to the selected profile list so
CI timing summaries record both process shapes as
`wordpress_perf_summary_mysqli_process_{explicit,implicit}_profile_embedded_open_*`.
The timing rollup preserves the same rows as
`wordpress_perf_mysqli_process_{explicit,implicit}_profile_embedded_open_*`.

## Compatibility Impact

No SQL, PHP mysqli API, public `libmylite` C API, WordPress behavior, wire
protocol, directory layout, or native storage semantics change. The new rows
are diagnostic-only output under an existing opt-in profile flag.

## Database Directory And Lifecycle Impact

No durable files are added. The profile observes the existing open/close
lifecycle and keeps both explicit `mysqli_close()` and PHP object-free close on
the supported `mylite_close()` path.

## Native Storage Impact

No InnoDB, MyISAM, Aria, redo, checkpoint, file lifecycle, or ownerless WAL
format changes. The metrics expose existing native startup and shutdown
subphase cost.

## Build, Size, And Dependency Impact

No new dependency. The PHP mysqli extension gains a small copied enum and
diagnostic print helpers. Runtime counter work remains disabled unless
`MYLITE_MYSQLI_PROFILE=1`.

## Test And Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`,
  `tools/wordpress-phpunit-timing-rollup`, and
  `tools/wordpress-phpunit-timing-rollup-test`.
- Run `ctest --preset php-embedded-prod -R '^php-ext-mysqli-mylite\.profile'`
  to prove the profile emits embedded-open rows.
- Run `tools/check-ci-production-builds` and its production CTest wrapper.
- Run a reduced production WordPress `perf-probe` with
  `MYLITE_WORDPRESS_PERF_PROFILE_CONNECT_PROCESSES=1` and verify the timing
  summary contains explicit and implicit embedded-open profile rows.
- Run `cmake --build --preset format-check-prod` and `git diff --check`.

## Verification Results

Local verification on 2026-06-22 used `build/php-embedded-prod` and the
production WordPress build path with `Release` MyLite and `MinSizeRel` MariaDB
embedded artifacts.

- `bash -n tools/wordpress-phpunit-mysqli-mylite`,
  `tools/wordpress-phpunit-timing-rollup`, and
  `tools/wordpress-phpunit-timing-rollup-test`: passed.
- `tools/wordpress-phpunit-timing-rollup-test`: passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_mysqli_php_extension`: passed after exporting the existing internal
  open-phase symbols for dynamic PHP extension resolution.
- `ctest --preset php-embedded-prod -R
  '^php-ext-mysqli-mylite\.profile' --output-on-failure`: passed.
- `tools/check-ci-production-builds`: passed.
- `ctest --preset prod -R
  '^tools\.(ci-production-builds|wordpress-phpunit-timing-rollup)$'
  --output-on-failure`: passed.
- `MYLITE_WORDPRESS_PHASE=build-php` with production guards rebuilt the
  WordPress PHP artifacts from `Release`/`MinSizeRel` inputs.
- A reduced production `prepare-db` plus `perf-probe` with
  `MYLITE_WORDPRESS_PERF_PROFILE_CONNECT_PROCESSES=1`,
  one process/connect iteration, ten SQL iterations, and two write iterations
  passed and appended embedded-open rows to
  `build/wordpress-embedded-open-profile/timing-summary.md`.
- `tools/wordpress-phpunit-timing-rollup --input
  build/wordpress-embedded-open-profile/timing-summary.md` preserved those
  rows in the final rollup.

The reduced perf-probe sample reported explicit process profile open
`202.935 ms` and close `53.819 ms`; the embedded open rows attributed
`196.080 ms` of open to runtime startup and `195.635 ms` to
`mysql_server_init()`. The implicit object-free sample reported open
`273.223 ms` and close `64.147 ms`, with `265.735 ms` of open in runtime
startup and `265.137 ms` in `mysql_server_init()`. Close-side cost was mostly
release/runtime shutdown: explicit `release_mysql_shutdown=51.397 ms` and
implicit `release_mysql_shutdown=61.375 ms`. In this reduced sample, embedded
connect and system-table checks were small relative to MariaDB startup and
shutdown.

## Acceptance Criteria

- The normal mysqli profile still prints existing open, close, query, and
  native-control rows.
- `MYLITE_MYSQLI_PROFILE=1` prints nonzero embedded open, start-runtime,
  close, and release-runtime counters.
- The WordPress perf-probe timing summary carries the new explicit and implicit
  process profile rows.
- The final timing rollup preserves the selected rows for CI artifact scanning.
- Production build guards remain intact.

## Risks And Follow-Up

The new values are timing evidence, not pass/fail thresholds. If the next CI
sample continues to show `start_mysql_server_init` and `release_mysql_shutdown`
as dominant, the next optimization should use the existing deeper MariaDB
startup/shutdown probes to target redo rebuild frequency, recovery bootstrap,
InnoDB system-table startup, or shutdown checkpoint behavior.
