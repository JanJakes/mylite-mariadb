# WordPress Mysqli Transaction Cache Retention

## Problem Statement

Production WordPress PHPUnit profiling shows repeated option-table result
queries interleaved with `START TRANSACTION` and `ROLLBACK`. The mysqli adapter
uses direct text execution for first-seen result queries and promotes immediate
exact repeats to the prepared-result cache, but transaction-control statements
cleared both the prepared-result cache and the exact-repeat promotion marker.
That prevented WordPress-shaped `SELECT; START; ROLLBACK; SELECT` sequences
from ever reaching the cached prepared-result path.

## Source Findings

- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c`
  `php_mylite_mysqli_query_impl()` classifies no-result statements before
  deciding whether to clear the result-query cache.
- `php_mylite_mysqli_no_result_query_preserves_cache()` previously preserved
  cached result statements only across no-result DML without `RETURNING`.
- MariaDB prepared statements are connection-local execution objects; retaining
  an already prepared result query across ordinary transaction boundaries does
  not cache rows or transaction visibility. A later reset failure still clears
  the internal cache and retries through a fresh prepare.
- The existing text-result route remains the default for first-seen and
  non-repeated result queries, preserving the broad WordPress unique-query
  improvement.

## Design

Split no-result cache retention into two decisions:

- preserve the prepared-result cache;
- preserve the immediate-repeat result SQL marker.

No-result DML without `RETURNING` keeps the prepared-result cache but clears the
immediate-repeat marker, so a post-DML result query still re-enters through the
text-result path unless it already has a prepared cache entry.

Exact transaction controls preserve both:

- `BEGIN` and `BEGIN WORK`;
- `START TRANSACTION`;
- `COMMIT` and `COMMIT WORK`;
- `ROLLBACK` and `ROLLBACK WORK`;
- the same forms with only optional trailing whitespace and a single semicolon.

Non-exact transaction forms such as `ROLLBACK TO SAVEPOINT`,
`COMMIT AND CHAIN`, `ROLLBACK RELEASE`, and option-bearing
`START TRANSACTION` continue to clear the cache conservatively. DDL, schema
changes, lock statements, `SET`, `USE`, `CALL`, explicit prepared statements,
reconnect, close, and error paths also keep the existing conservative clears.

## Compatibility Impact

No public PHP API, C API, SQL, storage, ownerless-locking, or directory
lifecycle behavior changes. The adapter retains prepared statement handles and
an exact SQL marker across transaction-control statements, but result rows are
still read at statement execution time under MariaDB's current transaction
visibility.

## Test Plan

- Extend the focused mysqli profile test with a repeated `SELECT` separated by
  `START TRANSACTION` and `ROLLBACK`, proving row correctness.
- Tighten the CTest profile regular expression so the test must emit nonzero
  `query_cache_hits` and `query_cache_preserved_no_result_calls`.
- Build the production PHP embedded target and run focused mysqli profile/API
  CTests.
- Run the production CI guard, formatting, tidy, and whitespace checks.
- Compare the focused WordPress DB profile counters when practical.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_mysqli_php_extension -j2` passed.
- `ctest --preset php-embedded-prod -R '^php-ext-mysqli-mylite\.(profile|api)$'
  --output-on-failure` passed.
- The verbose focused profile emitted `query_cache_hits=1`,
  `query_cache_misses=2`, `query_cache_preserved_no_result_calls=10`,
  `query_prepare_calls=2`, `query_transaction_start_calls=4`,
  `query_transaction_end_calls=4`,
  `libmylite_exec_result_native_control_start_transaction_calls=4`,
  `libmylite_exec_result_native_control_commit_calls=1`, and
  `libmylite_exec_result_native_control_rollback_calls=3`.
- `tools/check-ci-production-builds` passed.
- `cmake --build --preset format-check-prod` passed.
- `cmake --build --preset tidy-prod` passed.
- `git diff --check` passed.
- `MYLITE_WORDPRESS_PHASE=build-php ... tools/wordpress-phpunit-mysqli-mylite`
  refreshed the WordPress production artifacts after the freshness guard
  rejected the stale MariaDB embedded archive from the interrupted session.
  The rebuilt WordPress MariaDB archive used `MinSizeRel` and reported
  `size_mib=37.10`; the PHP extension build used `Release`.
- Focused production WordPress PHPUnit with
  `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`,
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, and `--filter '^Tests_DB'`
  passed `651` tests with `3` skips. It reported
  `wordpress_phpunit_reported_seconds=10.064`,
  `wordpress_phpunit_shell_real_seconds=22.357`,
  `query_cache_preserved_no_result_calls=1421`, `query_cache_hits=0`,
  `query_prepare_calls=0`, `query_cache_lookup_calls=0`,
  `query_ms_total=6948.686`, `exec_no_result_ms_total=3675.894`,
  `query_transaction_start_ms_total=896.816`,
  `query_transaction_end_ms_total=865.016`, and
  `libmylite_exec_result_native_control_ms_total=1747.709`.

The focused WordPress run confirms that the current default WordPress
`Tests_DB` path is dominated by MariaDB text-query and native transaction
execution rather than prepared-result cache lookup overhead. This slice still
keeps the prepared-result cache policy correct for eligible exact repeats
around transaction controls and leaves the broader performance target on native
transaction and result execution.
