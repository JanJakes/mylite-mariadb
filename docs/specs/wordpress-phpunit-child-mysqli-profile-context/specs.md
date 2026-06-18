# WordPress PHPUnit Child MySQLi Profile Context

## Problem Statement

The WordPress process-isolated PHPUnit attribution now splits total child
process runtime from child PHP script runtime, but the existing mysqli profile
aggregate still sums parent, setup, and child PHP profile blocks together.
That hides the specific question needed for the next performance decision:
how much of the child script bucket is MyLite `mylite_open()`,
`mylite_close()`, and query work inside the child process itself.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` emits one
  `mylite_mysqli_profile_*` block at PHP module shutdown when
  `MYLITE_MYSQLI_PROFILE=1` and profiled work ran in that PHP process.
- `MYLITE_MYSQLI_PROFILE_OUTPUT` already redirects profile blocks to a
  harness-owned file, so process-isolated child profiles do not reach child
  stderr and do not fail PHPUnit.
- `tools/wordpress-phpunit-mysqli-mylite` patches PHPUnit
  `DefaultPhpProcess::runProcess()` before `proc_open()` and can control the
  child environment passed to PHP.
- The existing aggregate parser sums selected `mylite_mysqli_profile_*`
  counters across all profile blocks in one PHPUnit phase, but it cannot tell
  which blocks came from PHPUnit child processes.

## Scope And Non-Goals

In scope:

- Add an optional sanitized `MYLITE_MYSQLI_PROFILE_CONTEXT` line to mysqli
  profile output.
- Tag PHPUnit child-process environments with
  `MYLITE_MYSQLI_PROFILE_CONTEXT=wordpress_phpunit_child` when mysqli
  profiling is enabled.
- Keep existing all-process `mylite_mysqli_profile_aggregate_*` rows.
- Add child-only `mylite_mysqli_profile_child_aggregate_*` rows for tagged
  profile blocks.

Out of scope:

- Changing PHPUnit filters, WordPress bootstrap, or process-isolated test
  behavior.
- Enabling mysqli profiling on normal CI timing paths.
- Optimizing `mylite_open()`, `mylite_close()`, query execution, or MariaDB
  startup/shutdown in this slice.

## Design

Extend the mysqli adapter profile output:

- read `MYLITE_MYSQLI_PROFILE_CONTEXT` at profile-print time;
- sanitize it to at most 64 key-safe characters using alphanumerics plus
  `_`, `-`, `.`, and `:`;
- print `mylite_mysqli_profile_context=<value>` immediately after the profile
  PID line and before additive counters.

Extend the WordPress PHPUnit patcher:

- add a distinct
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_MYSQLI_PROFILE_CONTEXT` marker so warmed
  PHPUnit vendor trees upgrade;
- before child `proc_open()`, set
  `MYLITE_MYSQLI_PROFILE_CONTEXT=wordpress_phpunit_child` in the child
  environment only when `MYLITE_MYSQLI_PROFILE=1`.

Extend the harness aggregate parser:

- reset the current profile context when a new
  `mylite_mysqli_profile_enabled=1` block starts;
- recognize `mylite_mysqli_profile_context=wordpress_phpunit_child`;
- continue summing all selected profile rows into the existing aggregate;
- additionally sum tagged rows into `mylite_mysqli_profile_child_aggregate_*`
  totals and derived averages.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, native storage, or WordPress test
behavior changes. The new profile context is emitted only when both
`MYLITE_MYSQLI_PROFILE=1` and `MYLITE_MYSQLI_PROFILE_CONTEXT` are set.

## Directory And Lifecycle Impact

No durable directory-layout change. The profile context is process-local
diagnostic output. Existing temporary profile-output files remain
harness-owned and are removed after summary extraction.

## Build, Size, License, And Dependencies

No dependency or license changes. The compiled mysqli adapter gains a small
dormant diagnostic formatter and the harness gains shell/awk post-processing
for explicitly profiled phases.

## Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Build the production PHP mysqli extension and run the profile/profile-context
  CTests.
- Run the WordPress `dependencies` phase on a warmed tree and verify the
  generated PHPUnit vendor file contains the child mysqli profile context
  marker.
- Run a focused production process-isolated WordPress PHPUnit method with
  child-process profiling and mysqli profiling enabled; verify child-script
  rows, all-process aggregate rows, and child-only aggregate rows.
- Run a matching unprofiled method and verify the profile rows are absent.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Completed on 2026-06-18 with the production `php-embedded-prod` and
`build/wordpress-php-embedded-prod` Release builds plus the
`build/wordpress-mariadb-embedded` MinSizeRel archive.

Syntax, extension build, and PHP profile tests passed:

```text
bash -n tools/wordpress-phpunit-mysqli-mylite

cmake --preset php-embedded-prod
cmake --build --preset php-embedded-prod --target mylite_mysqli_php_extension -j2

ctest --preset php-embedded-prod \
  -R '^php-ext-mysqli-mylite\.profile(-context)?$' --output-on-failure
100% tests passed, 0 tests failed out of 2
```

The guarded WordPress production PHP artifacts were refreshed after the
extension change:

```text
MYLITE_WORDPRESS_PHASE=build-php \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
tools/wordpress-phpunit-mysqli-mylite

mylite_php_build_seconds=8
mylite_build_seconds=9
```

The warmed PHPUnit vendor patch upgraded and the generated file contained the
new child context marker:

```text
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

A focused production process-isolated WordPress PHPUnit method passed with both
child-process and mysqli profiling enabled. The timing summary kept the
all-process aggregate and added child-only rows:

```text
MYLITE_WORDPRESS_PHASE=phpunit \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1 \
MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1 \
MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1 \
MYLITE_WORDPRESS_PHPUNIT_STATIC_WPDB_SCAN=0 \
MYLITE_WORDPRESS_TIMING_LABEL=manual-child-mysqli-context-profile \
tools/wordpress-phpunit-mysqli-mylite \
  --filter '^Tests_Formatting_Emoji::test_print_emoji_detection_script_on_front_end$'

OK (1 test, 4 assertions)
wordpress_phpunit_child_process_runtime_seconds=6.837140
wordpress_phpunit_child_script_seconds=6.050340
mylite_mysqli_profile_aggregate_processes=4
mylite_mysqli_profile_aggregate_open_ms_total=2866.511
mylite_mysqli_profile_aggregate_close_ms_total=3207.641
mylite_mysqli_profile_aggregate_query_ms_total=2370.606
mylite_mysqli_profile_child_aggregate_processes=2
mylite_mysqli_profile_child_aggregate_open_calls=3
mylite_mysqli_profile_child_aggregate_open_ms_total=1684.682
mylite_mysqli_profile_child_aggregate_close_calls=3
mylite_mysqli_profile_child_aggregate_close_ms_total=1338.260
mylite_mysqli_profile_child_aggregate_query_calls=477
mylite_mysqli_profile_child_aggregate_query_ms_total=1484.228
mylite_mysqli_profile_child_aggregate_open_ms_avg=561.561
mylite_mysqli_profile_child_aggregate_close_ms_avg=446.087
mylite_mysqli_profile_child_aggregate_query_ms_avg=3.112
```

A matching focused run with `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=0` passed
and its timing summary contained no `mylite_mysqli_profile_*` rows.

The production CI guard and format checks passed:

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

- Default unprofiled PHPUnit phases do not emit mysqli profile context or
  child aggregate rows.
- Profiled process-isolated PHPUnit phases tag child profile blocks without
  writing profile output to child stderr.
- Existing aggregate rows still sum all profile blocks.
- New child aggregate rows count only tagged child profile blocks and derive
  averages from child-only totals.
- Warmed PHPUnit vendor trees upgrade idempotently.

## Risks And Follow-Up

- The profile context identifies PHPUnit child processes, not individual test
  methods inside a child. That is enough for the current child-script
  attribution but not for per-test ranking.
- Child-only mysqli totals still measure adapter-boundary time, not individual
  MariaDB internal startup phases. If child open/close remains the dominant
  cost, the next slice should profile `mylite_open()` and `mylite_close()`
  subphases in `libmylite`.
