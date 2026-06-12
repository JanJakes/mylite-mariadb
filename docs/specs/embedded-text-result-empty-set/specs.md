# Embedded Text Result Empty Set

## Problem

The WordPress mysqli text-result fast path routes first-seen
`mysqli_query()` result SQL through `mylite_exec_result()` instead of the
prepared-statement path. `mylite_exec_result()` currently invokes its callback
only once per row. That preserves existing callback ergonomics, but it leaves
the adapter unable to distinguish a zero-row result set from a no-result
statement because no field metadata is delivered when `mysql_fetch_row()`
returns no rows.

For mysqli compatibility, `SELECT ... WHERE false` must still return a result
object with field metadata and `num_rows = 0`, not boolean `true`.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::store_and_emit_result()` calls
  `mysql_store_result()`, builds `mylite_exec_column` entries from
  `mysql_fetch_fields()`, and invokes the result callback only inside the
  `mysql_fetch_row()` loop.
- MariaDB's text result metadata is available before the first row through
  `mysql_num_fields()` and `mysql_fetch_fields()` on the stored result.
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c::
  php_mylite_mysqli_exec_query_impl()` currently treats an uninitialized field
  array after `mylite_exec_result()` as a no-result statement and returns
  boolean `true`.
- The existing `mylite_exec_result()` callback contract is row-oriented.
  Calling it for zero-row metadata would be a surprising behavior change for
  existing C callers.

## Design

Add an additive C API:

```c
typedef int (*mylite_exec_result_metadata_callback)(
    void *ctx,
    int column_count,
    const mylite_exec_column *columns);

int mylite_exec_result_with_metadata(
    mylite_db *db,
    const char *sql,
    mylite_exec_result_metadata_callback metadata_callback,
    mylite_exec_result_callback row_callback,
    void *ctx,
    char **errmsg);
```

`mylite_exec_result_with_metadata()` uses the same direct text execution path
as `mylite_exec_result()`. When a result set exists, it invokes the metadata
callback exactly once after field metadata is available and before any row
callback. For no-result statements, it does not invoke the metadata callback.
For row-producing result sets, row callbacks behave the same as
`mylite_exec_result()`.

Keep `mylite_exec_result()` unchanged by implementing it as a wrapper with no
metadata callback. `mylite_exec()` remains unchanged.

Update the mysqli adapter to use the metadata-aware API for the text-result
fast path. The metadata callback initializes the result field array even when
no rows are returned. The row callback keeps the same defensive field
initialization so it remains valid if a future caller uses the older API path.

## Scope And Non-Goals

In scope:

- Additive `libmylite` direct-result metadata API.
- mysqli zero-row direct text result compatibility.
- C and PHP coverage for empty result-set metadata.

Out of scope:

- Changing `mylite_exec_result()` row callback semantics.
- Changing prepared-statement result behavior.
- Changing result buffering, multi-result `CALL` draining, or ownerless
  visibility.

## Compatibility Impact

The public C API gains an additive function and callback typedef. Existing
`mylite_exec()` and `mylite_exec_result()` callers keep their current behavior.
The mysqli adapter becomes more MySQL/MariaDB-compatible for zero-row text
result sets while preserving the text-query performance path for unique result
SQL.

## Directory And Lifecycle Impact

No file, directory, or lifetime behavior changes. The new API uses the same
embedded connection and result-draining path as the existing direct execution
helpers.

## Native Storage Impact

No native storage behavior changes. This slice affects only text-result
metadata delivery after MariaDB has already executed the statement.

## Build And Performance Impact

The normal `mylite_exec_result()` path remains unchanged. The mysqli text-result
path adds one metadata callback per result set, replacing the previous
first-row-only field initialization. For non-empty result sets this moves field
initialization before row materialization; for empty result sets it fixes
behavior without falling back to prepared statements.

## Test And Verification Plan

- Add embedded C coverage for `mylite_exec_result_with_metadata()` on an empty
  result set, proving metadata callback delivery and zero row callbacks.
- Add API misuse coverage for the new function.
- Add PHP mysqli coverage that a zero-row `SELECT` returns a result object with
  field metadata and `num_rows = 0`.
- Run focused production C/PHP tests:
  `libmylite.api`, `libmylite.embedded-exec`, and
  `php-ext-mysqli-mylite.(api|profile)`.
- Run a reduced WordPress mysqli performance probe to ensure the text-result
  path still works under the production harness.
- Run production build guards, format check, and whitespace check.

## Verification Results

Local verification on 2026-06-12 used production build caches:
`build/embedded-prod`, `build/php-embedded-prod`, and
`build/wordpress-php-embedded-prod` with Release MyLite artifacts, plus
`build/mariadb-embedded` and `build/wordpress-mariadb-embedded` with the
MinSizeRel MariaDB embedded baseline.

- `cmake --build --preset php-embedded-prod --target mylite_api_test
  mylite_embedded_exec_test mylite_mysqli_php_extension` passed.
- `ctest --preset php-embedded-prod -R
  '^(libmylite\.api|libmylite\.embedded-exec|php-ext-mysqli-mylite\.(api|profile))$'
  --output-on-failure` passed.
- `ctest --preset php-embedded-prod -L php --output-on-failure` passed all
  four PHP integration tests.
- `cmake --build --preset embedded-prod --target
  mylite_ownerless_cross_process_sql_test` passed.
- Focused ownerless selectors `prepared-committed-read`,
  `local-write-first-read`, and `commit-race` passed.
- WordPress `build-php` with
  `MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod`,
  `MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release`,
  `MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1`, and
  `MYLITE_WORDPRESS_MARIADB_BUILD_DIR=build/wordpress-mariadb-embedded`
  passed with `mylite_build_seconds=31`.
- A reduced WordPress mysqli `perf-probe` with production guards,
  `MYLITE_WORDPRESS_PERF_PROCESS_ITERATIONS=2`,
  `MYLITE_WORDPRESS_PERF_CONNECT_ITERATIONS=2`,
  `MYLITE_WORDPRESS_PERF_SQL_ITERATIONS=1000`, and
  `MYLITE_WORDPRESS_PERF_WRITE_ITERATIONS=200` passed. It reported
  `wordpress_perf_summary_select1_ops_per_second=688.07`,
  `wordpress_perf_summary_point_select_ops_per_second=600.44`,
  `wordpress_perf_summary_insert_autocommit_ops_per_second=636.55`, and
  `wordpress_perf_summary_insert_direct_autocommit_ops_per_second=715.69`.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `tools/require-cmake-release-build build/embedded-prod
  build/php-embedded-prod build/wordpress-php-embedded-prod` passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded
  build/wordpress-mariadb-embedded` passed.
- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu
  cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- `mylite_exec_result()` remains row-callback compatible.
- `mylite_exec_result_with_metadata()` emits metadata for zero-row result sets
  and emits no row callbacks.
- mysqli direct text `SELECT` with no rows returns `MyLite\MySQLiResult` /
  `mysqli_result`, not boolean `true`.
- Existing direct result, binary-value, stored-procedure drain, and PHP mysqli
  profile tests still pass.

## Risks And Unresolved Questions

- The new API is public, so docs must be kept aligned with the implementation.
- Multi-result metadata beyond the first result set remains outside the current
  one-shot direct-result API; existing `CALL` draining behavior is unchanged.
