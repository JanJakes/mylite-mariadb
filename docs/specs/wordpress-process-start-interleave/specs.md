# WordPress Process Start Interleave

## Problem Statement

The WordPress mysqli `perf-probe` separates stock PHP process startup from PHP
startup with MyLite extensions loaded, then subtracts the former from the
latter to estimate extension-load overhead. The previous implementation
measured all stock PHP launches first and all MyLite-wrapper launches second.
Under host load or cold-cache effects this serial shape can make stock PHP look
slower than the MyLite wrapper and clamp the derived extension overhead to
zero.

That is bad performance evidence. It does not slow PHPUnit by itself, but it
can send debugging toward the wrong layer while the real WordPress cost remains
full embedded runtime open/close.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/wordpress-phpunit-mysqli-mylite` already alternates explicit-close and
  implicit-object-free process connect/close samples to avoid ordering bias.
- The same `perf-probe` phase previously measured stock `/usr/local/bin/php -r
  ''` process startup in one serial loop, then measured `${php_wrapper} -r ''`
  in a second serial loop.
- A fresh guarded local sample at `3d93fbfb` reported stock PHP process startup
  at `241.933 ms` and MyLite-wrapper PHP startup at `154.124 ms`, causing
  `wordpress_perf_summary_php_extension_process_overhead_ms_avg=0.000`. That
  contradicts earlier repeated samples where the MyLite wrapper usually adds a
  small positive process-start cost, and is consistent with ordering noise.

## Design

Measure stock PHP and MyLite-wrapper PHP process startup in alternating order:

- even iterations run stock PHP first, then the MyLite wrapper;
- odd iterations run the MyLite wrapper first, then stock PHP.

Accumulate nanosecond totals separately and keep the existing metric names:

- `wordpress_perf_stock_php_process_start_ms_avg`;
- `wordpress_perf_php_process_start_ms_avg`;
- `wordpress_perf_summary_stock_php_process_start_ms_avg`;
- `wordpress_perf_summary_php_process_start_ms_avg`;
- `wordpress_perf_summary_php_extension_process_overhead_ms_avg`.

Add an order marker to the detailed output and timing summary:

- `wordpress_perf_php_process_start_shape_order=alternating`;
- `wordpress_perf_summary_php_process_start_shape_order=alternating`.

Do not change process connect/close, in-process connect/close, active-runtime
reconnect, or SQL-loop measurements in this slice.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, storage, or runtime behavior
changes. The slice changes only performance-harness measurement order and one
additional summary marker.

## Directory And Lifecycle Impact

No database-directory behavior changes. The process-start samples still run
empty PHP snippets and do not open the MyLite database.

## Native Storage Impact

No native storage behavior changes.

## Build, Size, License, And Dependencies

No production binary, dependency, license, or package-size impact. The change is
limited to the Bash WordPress harness and documentation.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run a reduced guarded production `MYLITE_WORDPRESS_PHASE=perf-probe` and
  verify both process-start averages plus the new alternating-order summary
  marker are emitted and appended to the timing summary.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Acceptance Criteria

- Stock PHP and MyLite-wrapper PHP process startup samples are interleaved.
- Existing process-start metric names are preserved.
- The timing summary records the alternating process-start order marker.
- WordPress production-build audits continue to pass.

## Verification Results

Completed.

## Evidence

The shell syntax and whitespace checks passed:

```text
bash -n tools/wordpress-phpunit-mysqli-mylite
git diff --check
```

A reduced guarded production WordPress `perf-probe` passed:

```text
MYLITE_WORDPRESS_PHASE=perf-probe \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_TIMING_LABEL=manual-process-start-interleave \
MYLITE_WORDPRESS_TIMING_SUMMARY_PATH=build/manual-wordpress-process-start-interleave/timing-summary.md \
MYLITE_WORDPRESS_PERF_PROCESS_ITERATIONS=5 \
MYLITE_WORDPRESS_PERF_CONNECT_ITERATIONS=2 \
MYLITE_WORDPRESS_PERF_SQL_ITERATIONS=20 \
MYLITE_WORDPRESS_PERF_WRITE_ITERATIONS=3 \
tools/wordpress-phpunit-mysqli-mylite
```

It emitted the freshness marker and alternating process-start marker, and the
interleaved sample reported a positive extension-startup delta:

```text
wordpress_php_artifact_freshness_ok=/home/agent/.paseo/worktrees/1irk9sr4/greasy-ostrich/build/wordpress-php-embedded-prod
wordpress_perf_stock_php_process_start_ms_avg=75.418
wordpress_perf_php_process_start_ms_avg=94.014
wordpress_perf_php_process_start_shape_order=alternating
wordpress_perf_summary_php_extension_process_overhead_ms_avg=18.596
wordpress_perf_summary_php_process_start_shape_order=alternating
wordpress_perf_summary_php_process_connect_close_ms_avg=746.905
wordpress_perf_summary_mysqli_active_runtime_reconnect_ms_avg=3.558
wordpress_total_seconds=15
```

The timing summary recorded the new order marker:

```text
wordpress_perf_summary_php_process_start_shape_order=alternating
```

The production CI build-shape audits passed directly and through CTest:

```text
tools/check-ci-production-builds
ci_production_build_audit_ok=/home/agent/.paseo/worktrees/1irk9sr4/greasy-ostrich/.github/workflows/ci.yml

ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
100% tests passed, 0 tests failed out of 1
```

The format target passed:

```text
cmake --build --preset format-check-prod
```

## Risks And Follow-Up

- This is performance evidence hardening, not an engine optimization.
- The sample is still host-sensitive. Interleaving removes one ordering bias,
  but it does not make PHP process startup a deterministic benchmark.
- The remaining actionable performance problem is full embedded runtime
  open/close cost in short-lived PHP child processes; active-runtime reconnect
  and steady SQL rates remain much cheaper in current probes.
