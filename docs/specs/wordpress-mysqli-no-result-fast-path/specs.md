# WordPress MySQLi No-Result Fast Path

## Problem

The WordPress PHPUnit performance probe showed ordinary embedded C API reads in
the thousands of operations per second, while the PHP mysqli compatibility path
ran simple `mysqli_query()` reads and point selects in the low hundreds of
operations per second on the same host. A local branch/main comparison did not
show a branch regression in the ordinary engine path, but it did show that the
WordPress-shaped PHP adapter path is expensive enough to dominate full-suite
wall time.

The existing probe timed prepared inserts, but WordPress `wpdb` commonly sends
interpolated DML and DDL through `mysqli_query()`. A local direct-string insert
measurement showed this path was slower than the prepared insert metric and was
not visible in CI.

## Source Findings

- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c`
  `php_mylite_mysqli_query_impl()` used `mylite_prepare()`, `mylite_step()`,
  and `mylite_finalize()` for normal `mysqli_query()` statements.
- The prepared path preserves richer field metadata through
  `mylite_column_org_name()`, `mylite_column_table()`, and
  `mylite_column_org_table()`.
- `mylite_exec()` uses MariaDB `mysql_store_result()` and only exposes column
  names through its callback, so it cannot replace the prepared result-set path
  without weakening `mysqli_fetch_field()` metadata.
- For statements that do not return a result set, mysqli compatibility returns
  `true` plus connection status such as affected rows and insert id. Those
  statements do not need result metadata.
- WordPress `wpdb::query()` treats `CREATE`, `ALTER`, `TRUNCATE`, and `DROP`
  as boolean-result statements and `INSERT`, `DELETE`, `UPDATE`, and `REPLACE`
  as affected-row statements. Other statements are fetched as result sets.

## Design

Add a narrow no-result fast path to the MyLite mysqli adapter:

- Keep `CALL` on the existing `mylite_exec()` result-callback path, because
  stored procedures can return result sets and need result draining.
- Keep result-producing statements, including `SELECT`, `SHOW`, `DESCRIBE`,
  `EXPLAIN`, and unknown leading keywords, on the existing prepared path so
  field metadata remains intact.
- Route explicit no-result leading keywords through `mylite_exec()` without a
  callback, then refresh mysqli error, affected-row, and insert-id properties.
- Exclude `INSERT`, `UPDATE`, `DELETE`, and `REPLACE` statements containing a
  standalone `RETURNING` token so MariaDB result-producing DML remains on the
  prepared path.

Extend the WordPress `perf-probe` phase with
`wordpress_perf_insert_direct_autocommit_*` metrics that time direct
`mysqli_query()` inserts separately from prepared autocommit inserts.

## Compatibility Impact

No public API surface changes. Result-set query behavior remains on the
prepared path to preserve current field metadata. The fast path is limited to
SQL classes whose mysqli return value is already boolean or affected-row state.

The classifier is intentionally conservative. It only inspects the first SQL
token after leading whitespace and does not attempt to optimize comments,
common table expressions, or unknown SQL shapes.

## Directory And Lifecycle Impact

No durable layout changes. The WordPress probe creates and drops a temporary
`mylite_perf_probe_direct_autocommit` InnoDB table inside the prepared
WordPress test database directory.

## Build And Performance Impact

The adapter avoids prepared-statement setup for direct no-result
`mysqli_query()` DML/DDL. Local temporary measurements before the final scoped
implementation showed:

- direct string autocommit inserts improved from about `256.97 ops/s` to
  `323.67 ops/s` when routed through `mylite_exec()`;
- a broader experimental all-query `mylite_exec()` route improved simple reads
  from about `266.64 ops/s` to `856.53 ops/s`, but lost table/original-table
  field metadata and was rejected as too broad for compatibility.

The committed implementation only takes the no-result part of that experiment.

## Test Plan

- Run `bash -n tools/wordpress-phpunit-mysqli-mylite`.
- Build and run the PHP embedded mysqli extension test.
- Run the WordPress `perf-probe` phase with reduced iteration counts and verify
  the new direct autocommit insert metric is emitted.
- Run the CI-sized WordPress `perf-probe` locally when Docker and warmed build
  trees are available.
- Run `cmake --build --preset format-check`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-07 used the warmed WordPress Docker image and the
same `/tmp`-mounted database placement as the CI WordPress job.

- `bash -n tools/wordpress-phpunit-mysqli-mylite` passed.
- `cmake --build --preset php-embedded-dev --target
  mylite_mysqli_php_extension` passed.
- `ctest --preset php-embedded-dev -L php --output-on-failure` passed after the
  final formatted rebuild, including a mysqli regression check that
  `INSERT ... RETURNING` still returns a `MyLite\MySQLiResult` row instead of
  taking the no-result fast path.
- A reduced WordPress `perf-probe` with one process/connect iteration, five SQL
  iterations, and two write iterations passed and emitted
  `wordpress_perf_insert_direct_autocommit_ops_per_second=688.06`.
- A global mysqli replacement metadata check under the WordPress Docker wrapper
  passed with `name=id`, `orgname=id`, `table=one`, and `orgtable=one` for
  `SELECT id, name FROM one`, proving result-set queries stayed on the
  metadata-preserving path.
- A CI-sized local WordPress `perf-probe` with three process/connect
  iterations, 1000 SQL iterations, and 200 write iterations passed. It reported
  `wordpress_perf_select1_ops_per_second=263.34`,
  `wordpress_perf_point_select_ops_per_second=209.45`, prepared autocommit
  inserts at `360.00 ops/s`, and direct autocommit inserts at `557.36 ops/s`.
- `cmake --build --preset format-check` and `git diff --check` passed.

## Acceptance Criteria

- Direct no-result `mysqli_query()` statements avoid the prepared-statement
  path.
- Result-set `mysqli_query()` statements still preserve field name, original
  name, table, and original-table metadata.
- The WordPress performance probe prints
  `wordpress_perf_insert_direct_autocommit_iterations`,
  `wordpress_perf_insert_direct_autocommit_seconds`, and
  `wordpress_perf_insert_direct_autocommit_ops_per_second`.
- Focused PHP and harness checks pass.
