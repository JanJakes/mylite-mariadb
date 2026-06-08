# WordPress MySQLi Prepared Rebind Fast Path

## Problem

The WordPress mysqli performance probe now runs as a separate production CI
step and reports that prepared autocommit inserts remain slower than direct
no-result autocommit inserts. A local production probe on 2026-06-08 reported
prepared autocommit at `408.80 ops/s` and direct autocommit at `793.79 ops/s`.

One adapter-side cost is redundant native parameter clearing. Each
`mysqli_stmt_execute()` currently calls `mylite_reset()`, then
`mylite_clear_bindings()`, then immediately rebinds every PHP-bound parameter.
For normal `bind_param()` usage where the PHP binding count covers every native
parameter, the null clearing work is overwritten before execution.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c`
  `php_mylite_mysqli_stmt_execute_impl()` resets the native statement, clears
  every native binding, then binds all stored PHP values before stepping.
- `packages/libmylite/src/database.cc` rejects binding changes while a
  statement is executed, and `mylite_reset()` makes the statement bindable
  again before the adapter binds PHP values.
- `mylite_bind_parameter_count()` exposes the native parameter count already
  known by `libmylite`.

## Design

Skip `mylite_clear_bindings()` in `php_mylite_mysqli_stmt_execute_impl()` when
the stored PHP binding count is at least the native parameter count. Keep the
old clearing behavior for partial bindings so any unbound native parameter is
still reset to `NULL` before execution.

The existing `bind_param()` path continues to replace the adapter's stored PHP
bindings. Repeated executions still rebind every PHP value so by-reference
variables keep their current values.

## Compatibility Impact

No PHP API, SQL, public C API, storage, or ownerless behavior change. Correctly
bound prepared statements execute with the same native parameter values; partial
bindings keep the existing null-reset behavior.

## Directory And Lifecycle Impact

No durable file or directory-layout change.

## Build And Performance Impact

Prepared mysqli execution avoids one native null-bind pass per execution in the
common fully-bound case. This targets WordPress prepared-DML probe cost and does
not affect process startup or direct `mysqli_query()` DML.

## Test Plan

- Extend the mysqli API test to execute one bound prepared statement twice
  after mutating the bound PHP variables, proving repeated execution still uses
  current values after skipping the redundant clear.
- Build `mylite_mysqli_php_extension` with `php-embedded-prod`.
- Run the focused PHP mysqli CTest under `php-embedded-prod`.
- Run a reduced WordPress `perf-probe` and a CI-sized production
  `perf-probe`.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`,
  `cmake --build --preset format-check-prod`, and `git diff --check`.

## Verification Results

Local production verification on 2026-06-08:

- `cmake --build --preset format && cmake --build --preset php-embedded-prod
  --target mylite_mysqli_php_extension` passed.
- `ctest --preset php-embedded-prod -L php --output-on-failure` passed all 3
  PHP extension tests. The final post-revert production run completed in
  `5.92s`.
- `MYLITE_WORDPRESS_PHASE=build-php
  MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1
  MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod
  MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release
  tools/wordpress-phpunit-mysqli-mylite` passed, rebuilding the production
  WordPress PHP wrapper after the adapter change.
- A CI-sized WordPress `perf-probe` with 3 process/connect iterations, 1000 SQL
  iterations, and 200 write iterations passed. It reported stock PHP process
  startup `54.974ms`, PHP+MyLite startup `69.843ms`,
  process+connect `572.289ms`, active-runtime reconnect `3.510ms`, `SELECT 1`
  `243.53 ops/s`, point select `232.90 ops/s`, prepared autocommit inserts
  `363.51 ops/s`, and direct autocommit inserts `700.43 ops/s`.
- A write-heavy WordPress `perf-probe` with 1000 write iterations passed. It
  reported prepared autocommit inserts `373.78 ops/s` and direct autocommit
  inserts `696.01 ops/s`.
- A temporary experiment changing ordinary read-write opens to
  `--innodb-fast-shutdown=2` was rejected: production C-probe warm open/close
  stayed about `382.780ms` and `mysql_server_end()` still took about
  `249.580ms` per close.
- The final check chain
  `cmake --build --preset php-embedded-prod --target
  mylite_mysqli_php_extension mylite_embedded_performance_probe && ctest
  --preset php-embedded-prod -L php --output-on-failure && bash -n
  tools/wordpress-phpunit-mysqli-mylite && cmake --build --preset
  format-check-prod && git diff --check` passed.

The WordPress probe did not show a clear prepared-insert win from this
micro-optimization. The useful conclusion remains that process-isolated
WordPress cost is dominated by embedded MariaDB process lifetime, while
result-producing SQL and PHP adapter work dominate the in-process query loops.

## Acceptance Criteria

- Fully-bound prepared statements skip redundant native binding clears.
- Partial binding behavior stays conservative.
- Existing mysqli API coverage passes.
- WordPress performance probe still emits prepared and direct autocommit
  timings.
