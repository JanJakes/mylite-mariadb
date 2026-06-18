# WordPress mysqli Query Verb Attribution

## Problem Statement

The direct-execution attribution slice proved the focused WordPress `Tests_DB`
profile is dominated by native `mysql_query()` time inside libmylite direct
text execution. MyLite bookkeeping is not the next optimization target:
result draining, schema updates, and handle status updates were small.

The remaining performance question is which SQL classes are spending that
native execution time. Optimizing blindly inside `mysql_query()` is too broad;
MyLite first needs to know whether WordPress is paying mostly for DML, DDL,
transaction/control statements, connection-state statements, or result queries.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/php-ext-mysqli-mylite/src/php_mysqli_mylite.c` already has
  first-token SQL helpers for the mysqli adapter:
  `php_mylite_mysqli_is_call_query()`,
  `php_mylite_mysqli_is_no_result_query()`, and
  `php_mylite_mysqli_no_result_query_preserves_cache()`.
- The no-result classifier treats `INSERT`, `UPDATE`, `DELETE`, and
  `REPLACE` without `RETURNING` as no-result DML, and separately recognizes
  DDL, transaction, lock, `SET`, and `USE` classes.
- The existing `MYLITE_MYSQLI_PROFILE=1` path already records total query time,
  result/no-result counts, cache behavior, direct-exec totals, and libmylite
  direct-execution subphases.
- A local production `^Tests_DB` run on 2026-06-18 with the direct-exec profile
  reported main-process `query_calls=4010`, `query_ms_total=6735.347`,
  `exec_no_result_ms_total=4013.559`,
  `libmylite_exec_result_mysql_query_ms_total=6657.043`,
  `libmylite_exec_result_store_result_ms_total=99.029`,
  `libmylite_exec_result_current_schema_ms_total=5.782`, and
  `libmylite_exec_result_status_update_ms_total=0.254`.

## Design

Extend the opt-in mysqli profile with query-verb buckets. The adapter will
parse the first SQL keyword only when `MYLITE_MYSQLI_PROFILE=1` and attribute
the existing query elapsed time to one coarse class:

- `select`;
- `show` (`SHOW`, `DESCRIBE`, `DESC`, `EXPLAIN`);
- `dml` (`INSERT`, `UPDATE`, `DELETE`, `REPLACE`);
- `ddl` (`CREATE`, `ALTER`, `DROP`, `TRUNCATE`);
- `set`;
- `use`;
- `transaction` (`BEGIN`, `START`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`,
  `RELEASE`);
- `lock` (`LOCK`, `UNLOCK`);
- `call`;
- `other`.

Each bucket emits `query_verb_<name>_calls` and
`query_verb_<name>_ms_total`. The existing total query counters remain the
source of truth; verb buckets are a diagnostic split of those totals.

The WordPress timing summary and aggregate profile parser will carry the most
important verb rows into diagnostic summaries for profiled runs.

## Compatibility Impact

No SQL behavior, mysqli API behavior, public C API behavior, native storage
behavior, directory layout, or ownerless semantics change. The feature is
diagnostic-only and disabled unless `MYLITE_MYSQLI_PROFILE=1`.

## Directory And Lifecycle Impact

No durable files are created or moved. Profile counters are process-local and
printed at PHP module shutdown like the existing mysqli profile rows.

## Build, Size, License, And Dependencies

No new dependency or license impact. The extension gains small counter arrays
and first-keyword classification that runs only when profiling is enabled.

## Test And Verification Plan

- Build the production PHP mysqli extension target.
- Run `php-ext-mysqli-mylite.profile` under `php-embedded-prod` and require a
  nonzero query-verb profile key.
- Run the focused libmylite/PHP mysqli profile subset.
- Run a focused profiled production WordPress `^Tests_DB` sample and record the
  dominant query-verb buckets.
- Run `cmake --build --preset prod`, the production CI guard, format, tidy, and
  `git diff --check`.

## Acceptance Criteria

- Existing mysqli profile rows remain present.
- Profiled PHP mysqli output includes nonzero DML and DDL verb buckets in the
  existing profile test.
- Profiled WordPress timing summaries include query-verb rows.
- Unprofiled production timing remains unchanged except for compile-time size.

## Verification

Verified on 2026-06-18 with production builds:

- `cmake --build --preset php-embedded-prod --target mylite_mysqli_php_extension
  -j2` passed.
- `ctest --preset php-embedded-prod -R '^php-ext-mysqli-mylite\.profile$'
  -V` passed. The profile emitted `query_verb_select_calls=10`,
  `query_verb_dml_calls=2`, `query_verb_ddl_calls=2`,
  `query_verb_use_calls=1`, and retained
  `libmylite_exec_result_calls=14`.
- `ctest --preset php-embedded-prod -R
  '^(php-ext-mysqli-mylite\.(api|profile|profile-context))$'
  --output-on-failure` passed.
- A focused production WordPress `^Tests_DB` run with
  `MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1`,
  `MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1`, and
  `MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1` passed 651 tests with 3 skips.
  The main PHPUnit process reported `query_calls=4010`,
  `query_ms_total=6979.728`, `libmylite_exec_result_mysql_query_ms_total=6891.775`,
  `query_verb_select_ms_total=2571.350`,
  `query_verb_transaction_ms_total=1521.583`,
  `query_verb_ddl_ms_total=1514.698`,
  `query_verb_set_ms_total=960.696`, `query_verb_show_ms_total=250.494`,
  and `query_verb_dml_ms_total=157.285`.
- The same WordPress timing summary carried both last-profile and aggregate
  `query_verb_*` rows.
- `cmake --build --preset prod -j2`, `tools/check-ci-production-builds`,
  `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`, format check, `cmake --build --preset tidy-prod`, and
  `git diff --check` passed.
- Ownerless smoke
  `ctest --preset php-embedded-prod -R
  '^(libmylite\.ownerless-primitives|libmylite\.ownerless-cross-process-sql\.0|libmylite\.ownerless-single-owner-history-wal-proof)$'
  --output-on-failure` passed.

## Risks And Follow-Up

- The buckets intentionally group many SQL shapes. If a bucket dominates, the
  follow-up slice should add a narrower counter or optimization for that class.
- This does not optimize anything by itself; it identifies where the next
  behavior-preserving optimization should apply.
