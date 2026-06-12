# Embedded Text Query Drain Fast Path

## Problem

The production WordPress PHPUnit profile now shows startup and adapter status
work are no longer the long pole. The latest green CI run on this branch
reported the main non-isolated PHPUnit process at about `490753` mysqli query
calls, `393235 ms` of total query time, and only about `2521 ms` combined
open/close time. The default result-query path already uses
`mylite_exec_result()` for unique text SQL, so the next low-risk target is the
embedded one-shot query plumbing used by both result and no-result
`mysqli_query()` calls.

`store_and_emit_result()` drains any remaining MariaDB result sets after every
text statement. That is required for stored procedures and multi-result
statements, but the current drain loop calls `mysql_next_result()` even after
ordinary single-result statements just to receive `-1`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/libmysqld/libmysql.c:4857` implements
  `mysql_more_results()` as a local `SERVER_MORE_RESULTS_EXISTS` status check.
- `mariadb/libmysqld/libmysql.c:4871` implements `mysql_next_result()` and
  returns `-1` when that same status bit is not set.
- `mariadb/include/mysql.h:878` exposes `mysql_more_results()` in the embedded
  API used by MyLite.
- `packages/libmylite/src/database.cc::store_and_emit_result()` already
  centralizes text-query result draining for `mylite_exec()` and
  `mylite_exec_result()` in ordinary and ownerless modes.
- `packages/libmylite/tests/embedded_exec_test.c` already covers
  `mylite_exec()` over a stored-procedure `CALL`, then reuses the same handle
  for a normal query. That is the right compatibility boundary for this slice.

## Design

Guard `drain_remaining_query_results()` with `mysql_more_results()`:

- ordinary single-result statements return without calling `mysql_next_result()`;
- stored procedures and other multi-result statements continue to call
  `mysql_next_result()`, `mysql_store_result()`, and `mysql_free_result()` until
  MariaDB reports no remaining results;
- any positive `mysql_next_result()` error still maps to the existing MariaDB
  error handling.

This follows MariaDB's own C API contract and does not change SQL parsing,
statement execution, result materialization, affected rows, insert IDs, or
ownerless visibility.

## Compatibility Impact

No public API behavior changes. `mylite_exec()` and `mylite_exec_result()`
still drain stored-procedure/multi-result output before returning, and ordinary
single-result queries expose the same rows and status fields.

The embedded exec test is extended so a stored-procedure `CALL` is followed by
a write and a read on the same handle, proving no pending procedure result
state leaks across the optimized drain path.

## Directory And Lifecycle Impact

No durable files, directory layout, native storage files, ownerless WAL files,
or runtime lifecycle behavior change.

## Native Storage Impact

No native MariaDB storage format or InnoDB behavior change. This only removes a
redundant embedded client API call for the common single-result text-query
case.

## Build And Performance Impact

The expected win is small per query but broad: WordPress executes hundreds of
thousands of ordinary text statements in the non-isolated PHPUnit shard. Skipping
one no-more-results probe per statement should keep the PHP test body closer to
the native MariaDB text-query baseline without changing statement semantics.

## Test And Verification Plan

- Build the embedded production `mylite_embedded_exec_test`,
  `mylite_embedded_performance_probe`, and PHP mysqli extension targets.
- Run focused embedded exec coverage, including stored-procedure `CALL`.
- Run focused PHP mysqli API/profile coverage because the mysqli adapter uses
  `mylite_exec()` and `mylite_exec_result()` for hot `mysqli_query()` paths.
- Run a reduced production WordPress `perf-probe` if the warmed Docker/build
  cache is available.
- Run production build guards, format check, and diff whitespace checks.

## Verification Results

Local verification on 2026-06-12 used the active `ownerless-concurrency`
worktree with production build caches: `build/embedded-prod` and
`build/php-embedded-prod` were `Release`, and `build/mariadb-embedded` plus the
WordPress MariaDB cache were `MinSizeRel`.

- `cmake --build --preset embedded-prod --target mylite_embedded_exec_test
  mylite_embedded_performance_probe` passed.
- `cmake --build --preset php-embedded-prod --target
  mylite_mysqli_php_extension` passed.
- `build/embedded-prod/packages/libmylite/mylite_embedded_exec_test` passed.
- `ctest --preset php-embedded-prod -R
  '^(libmylite\.embedded-exec|php-ext-mysqli-mylite\.(api|profile))$'
  --output-on-failure` passed.
- A reduced production embedded performance probe with one open/close sample,
  `2000` select iterations, and `100` insert iterations passed. It reported
  ordinary direct `SELECT 1` at `4148.92 ops/s`, ownerless direct `SELECT 1` at
  `3621.50 ops/s`, ordinary autocommit inserts at `3354.09 ops/s`, ownerless
  autocommit inserts at `706.31 ops/s`, and ownerless autocommit ratio
  `0.2106`.
- `MYLITE_WORDPRESS_PHASE=build-php` with
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`,
  `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1`, and the guarded
  `build/wordpress-mariadb-embedded` cache rebuilt the WordPress PHP extension
  artifacts in `34s`.
- `MYLITE_WORDPRESS_PHASE=prepare-db` passed with the same production guards.
- A reduced guarded WordPress `perf-probe` with `3` process/connect samples,
  `2000` SQL iterations, and `400` write iterations passed. It reported
  `SELECT 1` at `806.08 ops/s`, point selects at `540.73 ops/s`,
  transactional inserts at `754.88 ops/s`, prepared autocommit inserts at
  `778.06 ops/s`, and direct autocommit inserts at `739.19 ops/s`.
- `cmake --build --preset embedded-prod --target
  mylite_ownerless_cross_process_sql_test` passed.
- Focused ownerless selectors `prepared-committed-read`,
  `local-write-first-read`, and `commit-race` passed.
- A reduced ownerless stress pass with `MYLITE_OWNERLESS_STRESS_ITERATIONS=20`
  and `MYLITE_OWNERLESS_STRESS_READER_POLLS=20` passed.
- `tools/check-ci-production-builds`, `ctest --preset prod -R
  '^tools\.ci-production-builds$' --output-on-failure`,
  `tools/require-cmake-release-build build/embedded-prod build/php-embedded-prod`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `cmake --build --preset format-check-prod`, and `git diff --check` passed.

## Acceptance Criteria

- Single-result `mylite_exec()`/`mylite_exec_result()` calls do not invoke
  `mysql_next_result()` only to learn there are no more result sets.
- Stored-procedure `CALL` result draining remains correct and leaves the
  connection usable for later write and read statements.
- Existing mysqli API/profile tests pass.
- Docs record that this is a query plumbing fast path, not a SQL or storage
  compatibility change.

## Risks And Unresolved Questions

- The speedup may be hidden by larger MariaDB execution costs on CI. The change
  is still justified because it removes one redundant embedded C API transition
  from the hottest WordPress path without broadening the adapter surface.
- Broader WordPress query-time work remains in MariaDB text execution,
  result-row materialization, and no-result DML execution. Ownerless autocommit
  still has separate sync and mini-transaction costs.
