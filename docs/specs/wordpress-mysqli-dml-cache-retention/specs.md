# WordPress MySQLi DML Cache Retention

## Problem

The expanded WordPress mysqli profile showed that the focused production
`Tests_DB` run spent materially more time in query prepare, result stepping,
and statement cache clear/finalize than in PHP fetch conversion. The same run
reported `query_cache_clear_finalize_calls=1615`,
`query_cache_clear_ms_total=3530.218`, `query_prepare_ms_total=4327.226`, and
`query_cache_hits=0` in the main process.

The current mysqli adapter clears the cached prepared result statement before
every no-result statement. That is necessary for DDL, schema changes,
transaction control, lock statements, `SET`, and `USE`, but it is more
conservative than needed for ordinary no-result DML. Re-executing the same
prepared `SELECT` after `INSERT`, `UPDATE`, `DELETE`, or `REPLACE` should see
the current rows without reparsing the statement or rebuilding result metadata.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` stores one cached
  result statement per mysqli link in `query_cache_stmt` with matching SQL in
  `query_cache_sql`.
- `php_mylite_mysqli_query_impl()` currently clears that cache before both
  `CALL` and every known no-result query.
- `php_mylite_mysqli_is_no_result_query()` already identifies
  `DELETE`, `INSERT`, `REPLACE`, and `UPDATE` as no-result only when the
  remaining SQL text does not contain a standalone `RETURNING` token.
- The same classifier treats `ALTER`, `BEGIN`, `COMMIT`, `CREATE`, `DROP`,
  `LOCK`, `RELEASE`, `ROLLBACK`, `SAVEPOINT`, `SET`, `START`, `TRUNCATE`,
  `UNLOCK`, and `USE` as no-result statements. These can affect metadata,
  schema, transaction state, lock state, or connection state and should keep
  the conservative cache clear.
- `packages/libmylite/src/database.cc` implements prepared statements with
  `mysql_stmt_prepare()`, `mysql_stmt_execute()`, and `mysql_stmt_fetch()`.
  Result metadata comes from `mysql_stmt_result_metadata()` in
  `initialize_statement_results()`, so preserving a prepared statement across
  ordinary DML retains the same metadata contract while re-executing against
  current table contents.
- `packages/php-ext-mysqli-mylite/tests/mysqli_api_test.php` already verifies
  that a repeated cached `SELECT` sees a newly inserted row after DML and that
  DDL replacement still produces the recreated table's row. The test did not
  previously prove whether the DML case reused the cached statement or
  re-prepared it.

## Design

Add a classifier helper that recognizes cache-preserving no-result DML:
`DELETE`, `INSERT`, `REPLACE`, and `UPDATE` without a standalone `RETURNING`
token after the first keyword.

In `php_mylite_mysqli_query_impl()`, keep the existing no-result execution path
but clear `query_cache_stmt` only when the no-result statement is not
cache-preserving DML. `CALL`, DDL, schema, transaction, lock, `SET`, `USE`, and
error paths continue to clear the cache as before. Result statements with
`RETURNING` stay on the result path.

Extend profile counters with `query_cache_preserved_no_result_calls` so CI can
show how often no-result DML kept a cached result statement alive. Extend the
PHP profile test with a repeated `SELECT` separated by `INSERT` so local
profile output proves the DML-preserved cache hit.

## Compatibility Impact

No public API or SQL result behavior changes. A prepared result query reused
after DML is re-executed through `mylite_reset()` and `mylite_step()`, so it
observes current data while retaining the same field metadata shape.

The conservative invalidation boundary remains for statement classes that can
change metadata, schema, connection state, transaction state, or locking
semantics.

## Directory And Lifecycle Impact

No durable files or directory lifecycle behavior changes. The slice only keeps
an already-open prepared statement alive longer inside one mysqli link.

## Build, Size, And Dependency Impact

No dependencies are added. The extension gains a small classifier helper and
one profile counter.

## Test Plan

- Build the production PHP mysqli extension target.
- Run `ctest --preset php-embedded-prod -R
  '^php-ext-mysqli-mylite\.(api|profile)$' --output-on-failure`.
- Run a focused production WordPress `Tests_DB` filter with
  `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1` and
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1` to compare cache-preserved DML,
  cache hits, finalizes, and query times.
- Run `tools/check-ci-production-builds`,
  `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`, `cmake --build --preset format-check-prod`, and
  diff checks.

## Acceptance Criteria

- Ordinary no-result `INSERT`, `UPDATE`, `DELETE`, and `REPLACE` without
  `RETURNING` do not clear the cached result statement.
- `RETURNING` DML remains result-producing and does not enter the no-result
  cache-preserving path.
- DDL, schema, transaction, lock, `SET`, `USE`, `CALL`, and error paths keep
  the previous conservative cache clear behavior.
- The PHP API test still passes for repeated cached SELECTs, DML refresh, DDL
  replacement, field metadata, and result fetching.
- The profile test emits nonzero `query_cache_preserved_no_result_calls` and a
  cache-hit count that proves a SELECT-DML-same-SELECT sequence reused the
  cached statement.

## Verification Results

Local verification on 2026-06-11 used production build caches:
`build/php-embedded-prod` with `Release` MyLite,
`build/wordpress-php-embedded-prod` with `Release` MyLite, and
`build/wordpress-mariadb-embedded` with `MinSizeRel` MariaDB embedded.

- `cmake --build --preset php-embedded-prod --target
  mylite_mysqli_php_extension` passed.
- `ctest --preset php-embedded-prod -R
  '^php-ext-mysqli-mylite\.(api|profile)$' --output-on-failure` passed.
- `ctest --preset php-embedded-prod -R '^php-ext-mysqli-mylite\.profile$' -V`
  passed and reported `query_cache_hits=2`,
  `query_cache_preserved_no_result_calls=2`, and
  `query_cache_clear_finalize_calls=4`, proving the profile test's
  `SELECT`-DML-same-`SELECT` sequence reused the cached prepared statement.
- `MYLITE_WORDPRESS_PHASE=build-php ... tools/wordpress-phpunit-mysqli-mylite`
  rebuilt the WordPress `Release` mysqli extension through the Docker-backed
  `/work` cache.
- `MYLITE_WORDPRESS_PHASE=prepare-db ... tools/wordpress-phpunit-mysqli-mylite`
  recreated the external WordPress MyLite test database with `Release` and
  `MinSizeRel` guards.
- Focused production WordPress PHPUnit with
  `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`,
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, and `--filter '^Tests_DB'`
  passed `651` tests with `3` skips. Compared with the previous focused
  attribution sample, `wordpress_phpunit_reported_seconds` moved from
  `26.509` to `19.314`, `wordpress_phpunit_shell_real_seconds` moved from
  `71.503` to `33.581`, `query_ms_total` moved from `19084.516` to
  `15296.434`, `query_prepare_calls` moved from `1615` to `1612`,
  `query_cache_hits` moved from `0` to `3`,
  `query_cache_clear_finalize_calls` moved from `1615` to `1612`, and the new
  `query_cache_preserved_no_result_calls` counter reported `111`.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Open Questions

- This is a one-entry cache policy improvement. It does not address repeated
  non-adjacent SELECTs separated by other SELECTs, which would require a
  separate bounded LRU design and broader invalidation review.
- The full WordPress non-isolated shard is still needed to quantify how much
  ordinary DML cache retention helps outside the focused `Tests_DB` filter.
