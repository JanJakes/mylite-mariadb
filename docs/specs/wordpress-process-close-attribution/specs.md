# WordPress Process Close Attribution

## Problem

The WordPress PHPUnit job now exposes production build, setup, perf-probe, and
test-only timings separately, but the process-isolated test classes still pay a
large per-child MyLite lifecycle cost. The current production probes already
show that active-runtime reconnect is cheap while full process connect/close is
expensive. The remaining ambiguity is whether the child process path is paying a
material extra cost because the probe uses an explicit `mysqli_close()` while
real process-isolated children usually leave the connection object to PHP
object cleanup at request shutdown.

Skipping `mylite_close()` from PHP object cleanup would not be a safe
optimization without a separate durability and lifecycle API: the extension's
object destructor currently clears the statement cache and performs a real
`mylite_close()`, and explicit `mysqli_close()` does the same close path. The
next useful performance slice is therefore attribution, not a semantic shortcut.

## Source Findings

- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` opens mysqli links
  through `mylite_open(path, MYLITE_OPEN_READWRITE | MYLITE_OPEN_CREATE, ...)`.
  The WordPress adapter therefore exercises the ordinary embedded runtime, not
  ownerless read/write opens.
- `MyLite_MySQLi::close()` and global `mylite_mysqli_close()` both clear the
  query cache and call `php_mylite_mysqli_profiled_close(...,
  PHP_MYLITE_MYSQLI_PROFILE_CLOSE_EXPLICIT)`.
- `php_mylite_mysqli_link_free()` clears the query cache, clears the recent
  result SQL, and calls `php_mylite_mysqli_profiled_close(...,
  PHP_MYLITE_MYSQLI_PROFILE_CLOSE_OBJECT_FREE)` when a live link remains.
- `php_mylite_mysqli_profiled_close()` always calls `mylite_close()`, so both
  explicit and object-free paths pay the same supported MyLite close semantics.
- `tools/wordpress-phpunit-mysqli-mylite` already has a `perf-probe` phase that
  reports stock PHP startup, MyLite-extension PHP startup, process plus
  explicit connect/close, in-process connect/close, active-runtime reconnect,
  and steady SQL rates under CI's production build guards.

## Design

Extend the WordPress `perf-probe` phase with a second bounded process shape that
connects to the prepared MyLite database and then exits without calling
`mysqli_close()`. PHP object cleanup still closes the link through
`php_mylite_mysqli_link_free()`, but the new measurement matches the
implicit-close shape used by process-isolated PHPUnit children.

Measure the explicit-close and implicit object-free shapes in alternating order
and average each shape separately. That avoids a misleading result where the
second shape in a serial loop gets all of the warmed database-directory and
host-cache effects.

Keep the existing `wordpress_perf_php_connect_process_ms_avg` and summary keys
as the explicit-close measurement for compatibility with existing log readers.
Add explicit names for both paths:

- `wordpress_perf_php_connect_process_explicit_close_ms_avg`
- `wordpress_perf_php_connect_process_implicit_object_free_ms_avg`
- `wordpress_perf_php_connect_process_implicit_object_free_delta_ms_avg`
- `wordpress_perf_php_connect_process_implicit_minus_explicit_ms_avg`
- `wordpress_perf_php_connect_process_close_shape_order=alternating`

The same aliases are emitted under `wordpress_perf_summary_*` and appended to
the WordPress timing summary so CI step summaries expose the process-level
startup/close attribution without requiring log scraping.

## Compatibility Impact

No SQL, PHP API, mysqli API, MyLite runtime, or database-directory behavior
changes. The added script exits normally and relies on the existing PHP object
destructor to close the live mysqli link.

## Directory And Lifecycle Impact

No lifecycle semantics change. The slice explicitly avoids treating process exit
as permission to skip `mylite_close()`.

## Build And Performance Impact

The CI WordPress perf probe runs one additional process loop using the existing
`MYLITE_WORDPRESS_PERF_CONNECT_ITERATIONS` bound. CI currently sets that bound
to five iterations, so the extra wall time is small relative to the full
WordPress job and produces direct evidence for the per-child close path.

A reduced production-shaped verification run using the CI-pinned WordPress ref,
`Release` MyLite build, `MinSizeRel` MariaDB embedded build, five process and
connect iterations, and tiny SQL/write loops reported:

- explicit process connect/close: `759.288 ms`;
- implicit object-free process connect/close: `617.743 ms`;
- implicit minus explicit: `-141.545 ms`;
- active-runtime reconnect: `3.644 ms`.

The implicit process-shutdown shape was not slower than explicit close in this
sample, so the remaining process-isolated PHPUnit cost stays attributed to full
embedded lifecycle startup/shutdown rather than a PHP destructor-specific tax.

## Test Strategy

- Shell syntax check for `tools/wordpress-phpunit-mysqli-mylite`.
- `tools/check-ci-production-builds` to keep production timing guards intact.
- A reduced production `MYLITE_WORDPRESS_PHASE=perf-probe` run with five
  process/connect iterations to verify the new keys are emitted by the real
  wrapper and PHP extension.

## Follow-Up: Native Profile Attribution

### Problem Statement

The WordPress PHPUnit job is now split into production build, dependency,
database-prep, performance-probe, and test-only phases, but the slow
process-isolated shards are still dominated by one short-lived PHP process per
test. Existing `perf-probe` output shows PHP startup, explicit
`mysqli_close()` process connect/close, implicit object-free process
connect/close, in-process connect/close, active-runtime reconnect, and SQL
throughput. That exposes the shape of the slowdown but not whether the process
connect/close cost is mostly native open, native close, or PHP teardown shape.

### Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` runs the WordPress `perf-probe`
  through the production PHP wrapper and already alternates stock/MyLite PHP
  process-start samples plus explicit/implicit process connect/close samples.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` exposes
  `MYLITE_MYSQLI_PROFILE=1`, `MYLITE_MYSQLI_PROFILE_CONTEXT`, and
  `MYLITE_MYSQLI_PROFILE_OUTPUT`. The profile output already records
  `open_*`, `close_*`, and close-kind counters for explicit close,
  object-free close, and reopen close.
- Explicit and implicit process connect/close wall timings must stay
  comparable to prior branch/main samples, so profiling cannot be injected into
  the main averaged loop without changing the measurement being trended.

### Design

Add an opt-in WordPress perf-probe profile pass controlled by
`MYLITE_WORDPRESS_PERF_PROFILE_CONNECT_PROCESSES=1`.

When enabled, the harness runs one extra explicit-close process and one extra
implicit-object-free process after the existing unprofiled alternating
connect/close loop. Each extra process enables `MYLITE_MYSQLI_PROFILE=1` and
writes to a temporary profile file. The harness parses the stable profile keys
and emits prefixed metrics:

- `wordpress_perf_mysqli_process_explicit_profile_open_*`;
- `wordpress_perf_mysqli_process_explicit_profile_close_*`;
- `wordpress_perf_mysqli_process_implicit_profile_open_*`;
- `wordpress_perf_mysqli_process_implicit_profile_close_*`.

The same keys are appended to the timing summary under
`wordpress_perf_summary_mysqli_process_*` so CI step summaries and artifacts
carry the close-kind profile without scraping raw logs. CI enables the opt-in
flag for the production `Run WordPress mysqli performance probe` step, and the
production-build audit guards that marker.

### Compatibility Impact

No SQL, PHP mysqli API, public C API, storage, ownerless runtime, or directory
layout behavior changes. The slice only adds diagnostic process samples to the
WordPress performance harness and CI timing summary.

### Performance Impact

The existing process startup and connect/close averages remain unprofiled and
unchanged. CI pays two additional short PHP processes in the WordPress
`perf-probe` step. Those processes are diagnostic only and are not included in
the main explicit/implicit process timing averages.

### Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run a reduced guarded production WordPress `perf-probe` with
  `MYLITE_WORDPRESS_PERF_PROFILE_CONNECT_PROCESSES=1` and verify explicit and
  implicit profile keys are emitted and written to the timing summary.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

### Acceptance Criteria

- CI keeps production WordPress perf-probe timings on Release/MinSizeRel
  artifacts and enables process close profiling explicitly.
- The main process timing averages remain unprofiled.
- The timing summary records explicit-close and implicit-object-free native
  open/close profile rows.
- Existing production-build audits pass.

### Verification Results

Verified on 2026-06-19 with the WordPress production PHP harness using a
fresh `build/wordpress-mariadb-embedded` `MinSizeRel` archive and
`build/wordpress-php-embedded-prod` `Release` MyLite build.

- `bash -n tools/wordpress-phpunit-mysqli-mylite tools/check-ci-production-builds`:
  passed.
- `tools/check-ci-production-builds`: passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`: passed.
- `MYLITE_WORDPRESS_PHASE=build-php ... tools/wordpress-phpunit-mysqli-mylite`:
  refreshed the WordPress production PHP artifacts after the stale embedded
  archive guard rejected the old local archive.
- Reduced guarded `MYLITE_WORDPRESS_PHASE=perf-probe` with
  `MYLITE_WORDPRESS_PERF_PROFILE_CONNECT_PROCESSES=1`,
  one process/connect iteration, five SQL iterations, and two write iterations:
  passed and appended the expected timing-summary rows.
- `cmake --build --preset format-check-prod`: passed.
- `git diff --check`: passed.

The reduced production probe reported:

- explicit process connect/close: `380.417 ms`;
- implicit object-free process connect/close: `275.553 ms`;
- implicit minus explicit: `-104.864 ms`;
- active-runtime reconnect: `3.544 ms`;
- explicit native profile: open `149.577 ms`, close `43.439 ms`,
  `close_explicit_calls=1`;
- implicit native profile: open `149.807 ms`, close `50.108 ms`,
  `close_object_free_calls=1`.

The profile rows confirm that the short-lived PHPUnit process cost is mostly
outside active-runtime reconnect, with native open around `150 ms` and native
close around `40-50 ms` in this sample. The remaining explicit process wall
time is PHP process setup and teardown around the native lifecycle.

### Risks And Follow-Up

- The profile samples are host-sensitive and should be used for attribution,
  not pass/fail thresholds.
- This is instrumentation, not an engine optimization. If the profile rows
  continue to show native close or native startup as dominant, the next slice
  should target the corresponding libmylite/MariaDB embedded phase.
