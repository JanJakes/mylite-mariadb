# WordPress MySQLi Query Cache Fast Path

## Problem

The WordPress PHPUnit timing work already separated build/setup phases from
test-only phases and moved timing jobs onto production builds. Current evidence
shows focused database PHPUnit is close to main, while process-isolated classes
remain dominated by PHP child-process startup plus full MariaDB embedded
open/close. That startup cost cannot be hidden without changing close and
directory-lock semantics.

The remaining WordPress mysqli probe still shows result-producing direct
`mysqli_query()` loops running far below the lower-level embedded C API. The
adapter materializes result rows and field metadata into PHP arrays before
returning a `MyLite\MySQLiResult`, but it prepares and finalizes the same
native statement on every repeated result query.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `.github/workflows/ci.yml` uses production `prod` and `php-embedded-prod`
  presets for CI timing jobs, and the WordPress job uses
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release` with
  `build/wordpress-php-embedded-prod`.
- `tools/wordpress-phpunit-mysqli-mylite` already splits Docker image build,
  WordPress fetch, PHP extension build, dependencies, database preparation,
  `perf-probe`, and PHPUnit test-only phases.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c`
  `php_mylite_mysqli_query_impl()` currently routes explicit no-result
  statements through `mylite_exec()`, but result-producing statements still call
  `mylite_prepare()`, drain all rows, collect field metadata, and
  `mylite_finalize()` for every call.
- `php_mylite_mysqli_result` owns materialized `rows` and `fields`; it does not
  own a native `mylite_stmt`, so a completed direct-query statement can remain
  on the link without changing result lifetime.
- `docs/api/libmylite-c-api.md` documents that `mylite_close()` returns
  `MYLITE_BUSY` while statements are active, and that column values are valid
  only until the next `mylite_step()`, `mylite_reset()`, or
  `mylite_finalize()`.

## Design

Add a one-entry result-query statement cache to each mysqli link:

- cache only the last direct result-query SQL string and its prepared
  `mylite_stmt`;
- on an exact SQL match, reset and re-execute the cached statement instead of
  preparing again;
- materialize rows and fields exactly as the existing prepared result path does,
  preserving original field name/table metadata;
- clear the cache before `CALL`, explicit no-result statements, explicit
  prepared statements, schema/charset helpers, reconnect, and close, so
  DDL/DML, default-schema changes, and connection lifecycle changes cannot
  leave an old active statement pinned;
- if a cached execution fails, drop the cached statement and retry once through
  a fresh prepare so stale prepared metadata does not turn a recoverable schema
  change into a persistent failure.

This is intentionally a one-entry cache. WordPress and the existing probe both
benefit from repeated exact reads such as `SELECT 1`, while broader SQL cache
policy, multi-entry eviction, and cross-statement invalidation are left out of
scope.

## Compatibility Impact

No public PHP API, C API, SQL, ownerless, or storage behavior changes are
intended. Result-set queries still use the metadata-preserving prepared
statement path. Explicit no-result direct queries still use the existing
`mylite_exec()` fast path.

`mylite_close()` must not become busy because of an internal cache; the adapter
must finalize the cached statement before closing or reopening the link.

## Directory And Lifecycle Impact

No durable directory-layout change. The cache is process-local and link-local.
It is finalized before `mylite_close()`, so directory locks and MariaDB embedded
runtime lifetime remain explicit.

## Build And Performance Impact

The fast path avoids repeated native prepare/finalize work for consecutive
identical result-producing direct queries. It should improve the WordPress
`SELECT 1` perf-probe metric and any WordPress runtime path that repeats the
same direct result SQL through one mysqli link.

It does not reduce process-isolated child startup, full embedded
`mysql_server_init()`/`mysql_server_end()` cost, ownerless page-version write
publication, or direct no-result DML cost.

## Test And Verification Plan

- Extend `packages/php-ext-mysqli-mylite/tests/mysqli_api_test.php` so repeated
  identical `mysqli_query()` calls observe changed table data and still allow
  clean `mysqli_close()`, with repeated field-metadata coverage under the
  global mysqli compatibility symbols where `mysqli_fetch_field()` is exposed.
- Build `mylite_mysqli_php_extension` with `php-embedded-prod`.
- Run PHP extension CTest under `php-embedded-prod`.
- Rebuild the WordPress PHP extension production build and run a reduced
  `perf-probe` plus a CI-sized `perf-probe` to check repeated-read behavior.
- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Run production build-type guards, `format-check-prod`, and `git diff --check`.

## Verification Results

Local verification on 2026-06-09 used production artifacts:
`build/php-embedded-prod` and `build/wordpress-php-embedded-prod` were
`Release`, while `build/mariadb-embedded` and
`build/wordpress-mariadb-embedded` were `MinSizeRel`.

- `cmake --build --preset php-embedded-prod --target
  mylite_mysqli_php_extension` passed after formatting.
- `ctest --preset php-embedded-prod -L php --output-on-failure` passed all
  three PHP extension tests.
- WordPress `build-php` with
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod`,
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`, and
  `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1` rebuilt the final
  `mysqli_mylite` production module. The warmed MariaDB archive step reported
  `MinSizeRel` and no native archive work; the MyLite PHP build took `8s`.
- WordPress `prepare-db` with
  `MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1` prepared the test database on
  host `/tmp`, mounted in the container as tmpfs.
- A reduced production WordPress `perf-probe` with one process/connect
  iteration, `200` SQL iterations, and `50` write iterations passed. It
  reported `SELECT 1` at `337.24 ops/s`.
- The final CI-sized production WordPress `perf-probe` with three
  process/connect iterations, `1000` SQL iterations, and `200` write iterations
  passed against the final rebuilt module. It reported stock PHP startup
  `59.323 ms`, PHP wrapper startup `87.172 ms`, process plus connect/close
  `604.162 ms`, in-process connect/close `441.568 ms`, active-runtime
  reconnect `3.160 ms`, `SELECT 1` `396.98 ops/s`, point selects
  `224.03 ops/s`, transactional inserts `398.21 ops/s`, prepared autocommit
  inserts `365.60 ops/s`, and direct autocommit inserts `598.22 ops/s`.
- A focused WordPress test-only PHPUnit phase,
  `--filter '^Tests_DB::test_bail$'`, passed without a build phase. PHPUnit
  reported `1.618s`, while the harness reported shell real `17.813s`, keeping
  test-body time separate from WordPress/PHPUnit bootstrap overhead.

## Acceptance Criteria

- Repeated identical direct result queries reuse the cached native statement.
- Explicit no-result statements invalidate the cache before execution.
- Cached statement failures retry once with a fresh prepare.
- Result metadata remains compatible with the current prepared result path.
- Link close and reconnect finalize cached statements before closing the
  underlying MyLite database handle.
