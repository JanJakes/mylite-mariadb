# libmylite Native Control Fast Path

## Problem Statement

The WordPress mysqli query-verb attribution slice showed the focused
production `Tests_DB` sample still spends substantial time inside native
`mysql_query()` for test-harness control statements. The main PHPUnit process
reported `query_verb_transaction_ms_total=1521.583` and
`query_verb_set_ms_total=960.696`. WordPress' PHPUnit base test case issues
`SET autocommit = 0`, `START TRANSACTION`, and `ROLLBACK` around tests, so a
large part of the CI cost is repeated transaction scaffolding rather than
application result fetching.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/include/mysql.h` exposes native C API control functions:
  `mysql_commit(MYSQL *)`, `mysql_rollback(MYSQL *)`, and
  `mysql_autocommit(MYSQL *, my_bool)`.
- `packages/libmylite/src/database.cc::exec_result_impl()` currently sends all
  direct text execution through `mysql_query()` before collecting affected
  rows, insert id, result shape, current schema, and MyLite status fields.
- The ownerless `exec_result_impl()` branch surrounds `mysql_query()` with
  SQL-policy tokens, statement locks, page-version refresh, dictionary DDL
  coordination, transaction-state bookkeeping, and page-log publication.
  Native control substitution in ownerless mode is therefore out of scope for
  this slice.
- `build/wordpress-develop/tests/phpunit/includes/abstract-testcase.php`
  issues `SET autocommit = 0`, `START TRANSACTION`, and `ROLLBACK` in the
  shared test lifecycle.

## Design

Add a non-ownerless direct-exec fast path for exact simple control statements:

- `COMMIT`;
- `ROLLBACK` but not `ROLLBACK TO ...`;
- `SET autocommit = 0` and `SET autocommit = 1`.

The matcher accepts leading/trailing ASCII whitespace and one optional trailing
semicolon. It deliberately rejects chained, release, savepoint, multi-assignment,
and expression-valued forms so unsupported shapes keep the existing
`mysql_query()` semantics.

On a match, `exec_result_impl()` calls the corresponding MariaDB C API
function, records a native-control profile bucket, marks the statement as a
no-result execution, and updates MyLite status fields as `changes=0` and the
current `mysql_insert_id()`. `START TRANSACTION` remains on the SQL text path
because there is no equivalent simple MariaDB C API call with identical SQL
semantics.

## Compatibility Impact

This is a behavior-preserving implementation detail for direct non-ownerless
execution. Supported SQL text remains the same. Unsupported or non-exact forms
continue through MariaDB SQL parsing. Ownerless read/write execution is
unchanged.

The public C API does not change. Existing undocumented profiling exports gain
native-control diagnostic counters used only by the PHP mysqli diagnostic
profile.

## Directory And Lifecycle Impact

No durable files are created or moved. The fast path operates on the active
embedded `MYSQL` handle for an already open MyLite database.

## Native Storage Impact

Transaction commit/rollback/autocommit behavior remains delegated to MariaDB's
native embedded connection and storage engines. The slice avoids ownerless
execution because native-control calls would otherwise need to reproduce
ownerless statement-lock and page-version lifecycle bookkeeping.

## Build, Size, License, And Dependencies

No new dependencies or license impact. The default embedded binary gains a
small statement classifier and profile counters.

## Test And Verification Plan

- Add focused libmylite embedded coverage for simple `SET autocommit`,
  `ROLLBACK`, and `COMMIT` through direct execution, including unchanged
  rollback semantics and status fields.
- Build the production embedded and PHP production targets.
- Run focused libmylite embedded execution/transaction tests and PHP mysqli
  profile tests.
- Run a focused profiled production WordPress `^Tests_DB` sample and compare
  transaction/SET timing plus the new native-control profile counters.
- Run ownerless smoke to ensure ownerless routing remains unaffected.
- Run production build, production CI guard, format, tidy, and
  `git diff --check`.

## Acceptance Criteria

- Exact simple control statements use the native-control profile bucket in
  non-ownerless direct execution.
- Non-exact control forms continue through `mysql_query()`.
- Existing transaction rollback/commit behavior remains covered.
- WordPress `Tests_DB` profile shows native-control calls and lower native
  `mysql_query()` time for the repeated control bucket.
- Ownerless smoke still passes.

## Verification Results

Completed locally on 2026-06-18 with production builds.

## Evidence

Focused production embedded/PHP build and direct coverage passed:

```text
cmake --build --preset php-embedded-prod --target mylite_embedded_exec_test mylite_mysqli_php_extension -j2
ctest --preset php-embedded-prod -R '^(libmylite\.embedded-exec|php-ext-mysqli-mylite\.profile)$' --output-on-failure
ctest --preset php-embedded-prod -R '^libmylite\.embedded-transactions-recovery$' --output-on-failure
ctest --preset php-embedded-prod -R '^(php-ext-mysqli-mylite\.(api|profile|profile-context))$' --output-on-failure
```

The verbose profile test emitted nonzero native-control rows:

```text
mylite_mysqli_profile_query_calls=18
mylite_mysqli_profile_query_verb_set_calls=2
mylite_mysqli_profile_query_verb_transaction_calls=1
mylite_mysqli_profile_libmylite_exec_result_calls=17
mylite_mysqli_profile_libmylite_exec_result_mysql_query_ms_total=27.997
mylite_mysqli_profile_libmylite_exec_result_native_control_calls=3
mylite_mysqli_profile_libmylite_exec_result_native_control_ms_total=4.926
mylite_mysqli_profile_libmylite_exec_result_native_control_errors=0
```

The WordPress production build cache was refreshed and the prepared database
was recreated with guarded `Release` MyLite and `MinSizeRel` MariaDB embedded
artifacts:

```text
MYLITE_WORDPRESS_REF=6ddfc9d9b532c6e95c1266165149815895e2eb56 \
MYLITE_WORDPRESS_PHASE=build-php \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_BUILD_TARGETS='mylite_mysqli_php_extension' \
tools/wordpress-phpunit-mysqli-mylite

MYLITE_WORDPRESS_REF=6ddfc9d9b532c6e95c1266165149815895e2eb56 \
MYLITE_WORDPRESS_PHASE=prepare-db \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
tools/wordpress-phpunit-mysqli-mylite
```

A focused profiled production `^Tests_DB` run with keepalive passed 651 tests
with 3 skips. The main PHPUnit process reported:

```text
wordpress_phpunit_reported_seconds=7.914
wordpress_phpunit_shell_real_seconds=17.438
mylite_mysqli_profile_query_calls=4010
mylite_mysqli_profile_query_ms_total=5432.945
mylite_mysqli_profile_libmylite_exec_result_mysql_query_ms_total=3946.376
mylite_mysqli_profile_libmylite_exec_result_native_control_calls=1310
mylite_mysqli_profile_libmylite_exec_result_native_control_ms_total=1404.955
mylite_mysqli_profile_libmylite_exec_result_native_control_errors=0
mylite_mysqli_profile_query_verb_transaction_ms_total=1367.677
mylite_mysqli_profile_query_verb_set_ms_total=864.744
```

The preceding query-verb attribution sample, before this fast path, reported
`wordpress_phpunit_reported_seconds=9.596`,
`mylite_mysqli_profile_query_ms_total=6979.728`, and
`mylite_mysqli_profile_libmylite_exec_result_mysql_query_ms_total=6891.775`.

A matching unprofiled production `^Tests_DB` run passed with
`wordpress_phpunit_reported_seconds=8.613` and
`wordpress_phpunit_shell_real_seconds=17.319`.

The remaining production, ownerless smoke, format, tidy, and whitespace checks
passed:

```text
cmake --build --preset prod -j2
tools/check-ci-production-builds
ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure
ctest --preset php-embedded-prod -R '^(libmylite\.ownerless-primitives|libmylite\.ownerless-cross-process-sql\.0|libmylite\.ownerless-single-owner-history-wal-proof)$' --output-on-failure
LD_LIBRARY_PATH=/tmp/clang-format-18-root/usr/lib/x86_64-linux-gnu cmake --build --preset format-check-prod
cmake --build --preset tidy-prod
git diff --check
```

## Risks And Follow-Up

- The fast path intentionally does not cover `START TRANSACTION`, `BEGIN`,
  `COMMIT AND CHAIN`, `ROLLBACK TO SAVEPOINT`, or general `SET` expressions.
- If the remaining transaction cost still dominates after this slice, the next
  investigation should compare MariaDB text `START TRANSACTION` cost against a
  broader, explicitly tested transaction API strategy rather than inferring
  equivalence.
