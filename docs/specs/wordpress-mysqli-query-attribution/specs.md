# WordPress MySQLi Query Attribution

## Problem

The WordPress PHPUnit keepalive slice removed repeated full embedded
open/close as the primary non-isolated-suite cost. The next production CI run
reported the main non-isolated PHPUnit process at `query_calls=490857` and
`query_ms_total=809026.198`, while open plus close time had fallen to about
`2.560` seconds.

The remaining profile was still too coarse. `query_prepare_ms_total`,
`query_result_execute_ms_total`, and `exec_no_result_ms_total` explained much
of the query body, but not the residual adapter cost, result fetch cost, cache
handling, status-property synchronization, or whether PHP row conversion was a
material part of the wall time.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` routes
  `mysqli_query()` through `php_mylite_mysqli_query_impl()`.
- The adapter classifies `CALL` statements into a `mylite_exec()` callback
  path, known no-result statements into `mylite_exec()` without a callback, and
  result-producing queries into a prepared `mylite_stmt` path.
- `packages/libmylite/src/database.cc` implements `mylite_exec()` with
  `mysql_query()` followed by `mysql_store_result()` in `store_and_emit_result()`.
- The same file implements `mylite_prepare()` in `prepare_impl()` with
  `mysql_stmt_init()`, `mysql_stmt_attr_set(STMT_ATTR_UPDATE_MAX_LENGTH)`,
  `mysql_stmt_prepare()`, and parameter initialization.
- `mylite_step()` executes prepared statements through `mysql_stmt_execute()`,
  `initialize_statement_results()`, and `mysql_stmt_fetch()`, with ownerless
  policy, refresh, dictionary, and page-visibility work around ownerless opens.
- `mylite_finalize()` releases statement results and calls
  `mysql_stmt_close()`.
- `packages/php-ext-mysqli-mylite/tests/mysqli_api_test.php` asserts
  `mysqli_fetch_field()` metadata including `orgname`, `table`, and
  `orgtable`. A shortcut that used the simpler `mylite_exec()` callback for
  ordinary `SELECT` results would lose this metadata, so it is not a safe
  WordPress performance shortcut without a separate metadata design.

## Design

Extend the existing opt-in `MYLITE_MYSQLI_PROFILE=1` adapter profile rather
than changing default runtime behavior.

The profile keeps all existing keys and adds subphase counters for:

- SQL query classification.
- result-statement cache lookup, clear, and statement finalization.
- prepared-result stepping.
- PHP row materialization during result execution.
- field metadata materialization.
- result object creation.
- public mysqli status-property synchronization.
- `mylite_exec()` callback materialization.
- `fetch_assoc()`, `fetch_array()`, `fetch_object()`, and `fetch_all()` counts
  and elapsed time, covering both namespaced object methods and
  procedural/global replacement calls.

The WordPress CI non-isolated shard already enables
`MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, so these keys become visible in
the existing production timing logs. Other runs keep profiling disabled unless
explicitly opted in.

## Compatibility Impact

No SQL behavior, mysqli API behavior, PHP class layout, public C API, or
directory lifecycle changes. The profile is still process-local, opt-in, and
printed at PHP module shutdown.

The adapter still uses the prepared-statement result path for ordinary result
queries so `mysqli_fetch_field()` continues to expose original table and column
metadata.

## Directory And Lifecycle Impact

No durable files are created or moved. Profiling stores only process-local
counters in the PHP extension.

## Build, Size, And Dependency Impact

No dependencies are added. The compiled extension grows by a small set of
integer counters and conditional `clock_gettime()` calls that run only when
`MYLITE_MYSQLI_PROFILE=1` is present.

## Test Plan

- Build the production PHP extension target.
- Run the `php-ext-mysqli-mylite.profile` CTest under the
  `php-embedded-prod` preset.
- Run a focused production WordPress `Tests_DB` filter with
  `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1` and
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`.
- Run the production CI-build audit and format/diff checks.

## Acceptance Criteria

- Existing profile keys remain present for comparison with prior CI logs.
- The profile emits nonzero fetch timing keys when result fetch methods are
  used.
- The PHP profile test exercises `fetch_assoc()`, `fetch_array()`,
  `fetch_object()`, and `fetch_all()` and fails unless the fetch-object
  profile key is emitted.
- Focused WordPress production evidence separates fetch conversion from
  query execution and cache/finalize costs.
- No result-query shortcut weakens `mysqli_fetch_field()` metadata.

## Verification Results

Local verification on 2026-06-11 used production build caches:
`build/php-embedded-prod` with `Release` MyLite,
`build/wordpress-php-embedded-prod` with `Release` MyLite, and
`build/wordpress-mariadb-embedded` with `MinSizeRel` MariaDB embedded. The
WordPress MyLite test database lived under `/tmp`, mounted into the container
as tmpfs outside the repository worktree.

- `cmake --build --preset php-embedded-prod --target
  mylite_mysqli_php_extension` passed.
- `ctest --preset php-embedded-prod -R '^php-ext-mysqli-mylite\.profile$' -V`
  passed. The profile test reported `fetch_assoc_calls=1`,
  `fetch_array_calls=1`, `fetch_object_calls=1`, and `fetch_all_calls=1`.
- `MYLITE_WORDPRESS_PHASE=build-php ... tools/wordpress-phpunit-mysqli-mylite`
  rebuilt the WordPress `Release` mysqli extension through the Docker-backed
  `/work` cache.
- `MYLITE_WORDPRESS_PHASE=prepare-db ... tools/wordpress-phpunit-mysqli-mylite`
  recreated the external WordPress MyLite test database with `Release` and
  `MinSizeRel` guards.
- Focused production WordPress PHPUnit with
  `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`,
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, and `--filter '^Tests_DB'`
  passed `651` tests with `3` skips. The main process reported
  `wordpress_phpunit_reported_seconds=26.509`,
  `wordpress_phpunit_shell_real_seconds=71.503`, `query_calls=4010`,
  `query_ms_total=19084.516`, `query_prepare_ms_total=4327.226`,
  `query_result_execute_ms_total=4967.714`,
  `query_result_step_ms_total=4828.470`,
  `query_result_row_materialize_ms_total=80.108`,
  `query_cache_clear_ms_total=3530.218`,
  `query_cache_clear_finalize_calls=1615`,
  `exec_no_result_ms_total=6329.991`, `fetch_object_calls=76626`, and
  `fetch_object_ms_total=122.798`.
- `ctest --preset php-embedded-prod -R
  '^php-ext-mysqli-mylite\.(api|profile)$' --output-on-failure` passed after
  the final extension rebuild.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Open Questions

- The focused `Tests_DB` run is not a full-suite substitute. It proves that
  fetch conversion is not the long pole in that filter, while the full
  non-isolated shard is still needed to quantify the same buckets across all
  WordPress tests.
- The query cache had zero hits in the focused main process and nontrivial
  finalize time, but it has explicit compatibility coverage for repeated
  result queries after DML and DDL. Changing the cache policy belongs in a
  separate optimization slice after full-shard evidence confirms the same
  pattern.
- The main performance target now appears to be MariaDB/libmylite execution
  and prepared-statement lifecycle cost, not PHP fetch-object conversion.
