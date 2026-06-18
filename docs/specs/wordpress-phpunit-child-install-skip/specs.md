# WordPress PHPUnit Child Install Skip

## Problem Statement

Child-only mysqli profile context showed that a focused process-isolated
WordPress PHPUnit method includes child profile blocks with the same
install-like query shape as the parent bootstrap. WordPress
`tests/phpunit/includes/bootstrap.php` runs `install.php` unless
`WP_TESTS_SKIP_INSTALL=1`, so process-isolated children can pay a full
WordPress test install before executing a single method.

The WordPress database is already prepared by the dedicated CI database phase
and by the parent PHPUnit bootstrap. The process-isolated CI shards should not
reinstall WordPress inside each child unless the selected tests require that
behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- WordPress ref used by CI:
  `6ddfc9d9b532c6e95c1266165149815895e2eb56`.
- WordPress `tests/phpunit/includes/bootstrap.php` runs
  `tests/phpunit/includes/install.php` when
  `getenv('WP_TESTS_SKIP_INSTALL') !== '1'`.
- WordPress `install.php` drops the test tables and runs `wp_install()`, which
  is visible in the mysqli profile as hundreds of setup queries plus a full
  MyLite open/close.
- PHPUnit `DefaultPhpProcess::runProcess()` receives the child environment at
  the same `proc_open()` boundary already patched by MyLite for parent lock
  release, child timing, and child mysqli profile context.
- The CI process-isolated filters are explicit method/class lists, while the
  database suite and non-isolated suite remain separate steps.

## Scope And Non-Goals

In scope:

- Add `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL` to the WordPress harness.
- When enabled, set `WP_TESTS_SKIP_INSTALL=1` only in PHPUnit child process
  environments.
- Keep the global default disabled.
- Enable the flag only for the eager-reconnect process-isolated CI test-only
  step after focused and shard verification.
- Audit CI so the flag does not silently disappear from isolated steps or
  spread to database/non-isolated steps.

Out of scope:

- Changing WordPress source files or PHPUnit annotations.
- Skipping the top-level WordPress install during database preparation or
  parent PHPUnit bootstrap.
- Enabling child mysqli profiling or child-process timing on normal CI runs.

## Design

Extend `tools/wordpress-phpunit-mysqli-mylite`:

- parse and validate `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL` as `0` or
  `1`;
- forward it into the Docker container and print the selected value in the
  harness resource block;
- add a warmed-vendor marker
  `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL_ENV`;
- before PHPUnit child `proc_open()`, clone the child environment when needed
  and set `WP_TESTS_SKIP_INSTALL=1` only when the new MyLite harness flag is
  enabled.

Update `.github/workflows/ci.yml`:

- keep `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL: "0"` in the job defaults;
- set it to `"1"` only on the eager-reconnect process-isolated PHPUnit step.

Update `tools/check-ci-production-builds`:

- require the default disabled setting;
- require the enabled setting on the eager-reconnect process-isolated step;
- forbid the enabled setting on the deferred-reconnect, database, and
  non-isolated PHPUnit steps.

## Compatibility Impact

No SQL, PHP API, mysqli API, public C API, native storage, or application
runtime behavior changes. This is a WordPress PHPUnit harness optimization for
process-isolated test children.

The behavior relies on WordPress' documented test-bootstrap environment gate
for skipping install. It is applied only after the parent process has already
prepared the test database and only for the explicit eager-reconnect
process-isolated CI filter verified by this slice.

## Directory And Lifecycle Impact

No durable directory-layout changes. Skipping child install avoids repeated
drop/recreate work in the existing WordPress MyLite test directory; parent
open/close and child open/close semantics remain unchanged.

## Build, Size, License, And Dependencies

No compiled-code, binary-size, license, or dependency changes. The slice changes
shell/PHP harness patching and CI/audit configuration.

## Verification Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite
  tools/check-ci-production-builds`.
- Run the `dependencies` phase and verify the generated PHPUnit vendor file
  contains `MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL_ENV`.
- Run a focused production process-isolated WordPress PHPUnit method with
  child install skip, child-process profiling, and mysqli profiling enabled;
  verify the child install-like profile block disappears and the method still
  passes.
- Run the CI deferred-reconnect process-isolated filter with child install skip
  as a negative rollout check.
- Run the CI eager-reconnect process-isolated filter with child install skip.
- Run `tools/check-ci-production-builds`.
- Run `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Completed on 2026-06-18 with the guarded
`build/wordpress-php-embedded-prod` Release build and
`build/wordpress-mariadb-embedded` MinSizeRel archive.

Syntax and warmed-vendor patch checks passed:

```text
bash -n tools/wordpress-phpunit-mysqli-mylite
bash -n tools/check-ci-production-builds

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

The generated PHPUnit vendor file contains
`MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL_ENV` and sets
`WP_TESTS_SKIP_INSTALL=1` only in child environments when the MyLite harness
flag is enabled.

A focused production process-isolated emoji method passed with child install
skip, child-process profiling, and mysqli profiling enabled. Compared with the
prior profiled child-context run without install skip, the child install-like
profile block disappeared and child work dropped sharply:

```text
MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1 \
MYLITE_WORDPRESS_PHPUNIT_PROFILE_CHILD_PROCESSES=1 \
MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1 \
tools/wordpress-phpunit-mysqli-mylite \
  --filter '^Tests_Formatting_Emoji::test_print_emoji_detection_script_on_front_end$'

OK (1 test, 4 assertions)
wordpress_phpunit_child_process_runtime_seconds=2.156660
wordpress_phpunit_child_script_seconds=1.776197
mylite_mysqli_profile_child_aggregate_processes=1
mylite_mysqli_profile_child_aggregate_open_calls=2
mylite_mysqli_profile_child_aggregate_query_calls=32
mylite_mysqli_profile_child_aggregate_query_ms_total=85.622
```

The deferred-reconnect CI filter failed when child install skip was enabled,
so it remains on the default child install behavior:

```text
MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0 \
MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1 \
tools/wordpress-phpunit-mysqli-mylite --filter "$deferred_filter"

ERRORS!
Tests: 31, Assertions: 71, Errors: 2, Warnings: 5, Skipped: 1.
```

The same deferred-reconnect filter passed with the new flag disabled:

```text
MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=0 \
MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=0 \
tools/wordpress-phpunit-mysqli-mylite --filter "$deferred_filter"

WARNINGS!
Tests: 31, Assertions: 81, Warnings: 5, Skipped: 1.
wordpress_phpunit_reported_seconds=164.933
```

The eager-reconnect CI filter passed with child install skip enabled:

```text
MYLITE_WORDPRESS_PHPUNIT_RECONNECT_AFTER_CHILD=1 \
MYLITE_WORDPRESS_PHPUNIT_CHILD_SKIP_INSTALL=1 \
tools/wordpress-phpunit-mysqli-mylite --filter "$eager_filter"

OK (22 tests, 67 assertions)
wordpress_phpunit_reported_seconds=56.294
wordpress_phpunit_shell_real_seconds=70.917
```

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

- Default harness behavior preserves child install unless the new flag is set.
- The new flag affects only child process environments, not the parent PHPUnit
  bootstrap.
- The explicit CI eager process-isolated filter passes with child install
  skipped.
- CI audit prevents accidental rollout to deferred, database, or non-isolated
  PHPUnit steps.
- Timing summary still reports production build and PHPUnit phase metrics.

## Risks And Follow-Up

- Skipping install inside child processes assumes the selected process-isolated
  tests do not require per-child database reinstallation. The deferred filter
  failed that check and remains on normal child install behavior.
- If future process-isolated filters add database-mutating tests that do not
  clean up through normal WordPress test teardown, the audit and focused filter
  docs should be updated with new evidence before keeping the flag enabled.
