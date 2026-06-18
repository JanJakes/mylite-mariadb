# WordPress PHPUnit Child Script Attribution

## Problem Statement

The WordPress PHPUnit process-isolated shards are still the visible PHPUnit
pain point. Current profiling separates parent lock release, parent reconnect,
and total child-process runtime, but the child runtime bucket still mixes PHP
process startup, PHPUnit child job bootstrap, WordPress bootstrap, MyLite
ordinary open/close, and the test body.

That makes the next performance decision ambiguous. The branch should not spend
more time on SQL-loop micro-optimizations if the process-isolated wall time is
mostly child bootstrap and embedded runtime startup/shutdown.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The CI-pinned WordPress ref is
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- `tools/wordpress-phpunit-mysqli-mylite` patches PHPUnit's
  `vendor/phpunit/phpunit/src/Util/PHP/DefaultPhpProcess.php` during the
  `dependencies` phase.
- PHPUnit `DefaultPhpProcess::runJob()` receives the generated child PHP job
  before writing it to a temporary file, and `runProcess()` owns the matching
  `proc_open()` and `proc_close()` timing boundary.
- PHPUnit treats child `stderr` as a test error, so child-side timing cannot be
  printed directly from the child process.
- Existing parent-side child profiling is opt-in through
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1`, and default CI timing
  keeps that mode disabled except where the workflow intentionally enables
  process-isolated child summary rows.

## Scope And Non-Goals

In scope:

- When child-process profiling is enabled, wrap the generated child PHP job
  with a shutdown timer after the opening `<?php` tag.
- Pass a per-child timing file path through the child environment so the child
  writes timing data without touching stdout or stderr.
- Aggregate child-script timing in the parent and append summary rows to the
  existing timing summary when present.
- Keep the patch idempotent for warmed PHPUnit vendor trees.

Out of scope:

- Changing WordPress PHPUnit filters, coverage, reconnect policy, or keepalive
  policy.
- Enabling child profiling for default CI timing paths that currently keep it
  disabled.
- Optimizing MariaDB startup/shutdown or MyLite open/close in this slice.

## Design

Extend the PHPUnit patcher in `tools/wordpress-phpunit-mysqli-mylite`:

- inject a small child-job prelude in `DefaultPhpProcess::runJob()` only when
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES` is not `0`;
- create one temporary timing file per child in `runProcess()`, add the path to
  the child environment as
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SCRIPT_TIMING_FILE`, and remove the file
  after `proc_close()`;
- have the child shutdown function write one elapsed-seconds value to that
  file;
- aggregate `child_script_count`, `child_script_seconds`, and
  `child_outer_minus_script_seconds` into the parent stats;
- print and summarize:
  `wordpress_phpunit_child_script_count`,
  `wordpress_phpunit_child_script_seconds`,
  `wordpress_phpunit_child_process_outer_minus_script_seconds`,
  `wordpress_phpunit_child_script_ms_avg`, and
  `wordpress_phpunit_child_process_outer_minus_script_ms_avg`.

The child script timer starts after PHP has loaded the generated child job and
therefore excludes binary startup before the PHP file begins executing. The
outer-minus-script metric approximates process startup/teardown and parent
`proc_open()`/pipe overhead around the measured child script.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, native storage, or WordPress test
behavior changes. This is opt-in timing attribution for the WordPress PHPUnit
harness.

## Directory And Lifecycle Impact

No durable layout changes. The per-child timing file is a temporary file under
the container temp directory and is removed by the parent harness after the
child exits.

## Build, Size, License, And Dependencies

No compiled-code, binary-size, license, or dependency changes. The harness
patcher and timing summary post-processing remain shell/PHP code.

## Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run the `dependencies` phase on a warmed WordPress PHPUnit vendor tree and
  verify the child-script timing markers are installed.
- Run one focused process-isolated WordPress PHPUnit method with
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1` and verify raw output
  and the timing summary contain the new child-script rows.
- Run a matching unprofiled method and verify the new rows are absent.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `git diff --check`.

## Verification Results

Completed on 2026-06-18 with the production
`build/wordpress-php-embedded-prod` Release build and the
`build/wordpress-mariadb-embedded` MinSizeRel archive.

Syntax, dependency-patch, and generated PHP checks passed:

```text
bash -n tools/wordpress-phpunit-mysqli-mylite

MYLITE_WORDPRESS_PHASE=dependencies \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
tools/wordpress-phpunit-mysqli-mylite

php -l build/wordpress-phpunit-tools/vendor/phpunit/phpunit/src/Util/PHP/DefaultPhpProcess.php
No syntax errors detected
```

The first focused PHPUnit attempt correctly refused stale production WordPress
PHP artifacts after the latest native code changes, so the production WordPress
PHP extension build was refreshed:

```text
MYLITE_WORDPRESS_PHASE=build-php \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
tools/wordpress-phpunit-mysqli-mylite

mylite_php_build_seconds=31
mylite_build_seconds=34
```

A focused production process-isolated WordPress PHPUnit method passed with
child profiling enabled and emitted the new child-script attribution:

```text
MYLITE_WORDPRESS_PHASE=phpunit \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1 \
MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1 \
MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=0 \
MYLITE_WORDPRESS_TIMING_LABEL=manual-child-script-profile \
tools/wordpress-phpunit-mysqli-mylite \
  --filter '^Tests_Formatting_Emoji::test_print_emoji_detection_script_on_front_end$'

OK (1 test, 4 assertions)
wordpress_phpunit_child_process_count=1
wordpress_phpunit_child_process_runtime_seconds=4.335419
wordpress_phpunit_child_script_count=1
wordpress_phpunit_child_script_seconds=3.950969
wordpress_phpunit_child_process_outer_minus_script_seconds=0.384450
wordpress_phpunit_child_script_ms_avg=3950.969
wordpress_phpunit_child_process_outer_minus_script_ms_avg=384.450
```

The timing summary contained those same child-script rows under the
`manual-child-script-profile` label. A matching focused run with
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=0` passed and emitted no
`wordpress_phpunit_child_*` or `wordpress_phpunit_child_script_*` rows.

The CI production-build and formatting guards also passed:

```text
tools/check-ci-production-builds
ci_production_build_audit_ok=.github/workflows/ci.yml

cmake --preset prod
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
100% tests passed, 0 tests failed out of 1

cmake --build --preset format-check-prod
git diff --check
```

## Acceptance Criteria

- Default unprofiled PHPUnit phases do not emit child-script timing rows.
- Opt-in child-profile phases emit child-script count, child-script seconds,
  outer-minus-script seconds, and per-child averages without causing child
  stderr errors.
- Warmed PHPUnit vendor trees upgrade idempotently.
- CI production-build and timing guards continue to pass.

## Risks And Follow-Up

- Child-script timing still includes WordPress bootstrap, MyLite ordinary
  open/close, and the test body. It is narrower than total child process time,
  but it is not a pure MyLite engine metric.
- If outer-minus-script remains small, the next work should target child
  script startup/open-close or reduce process-isolated child count. If it is
  large, investigate PHP binary startup and PHPUnit process launch overhead.
