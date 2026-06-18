# libmylite Autocommit No-Op Fast Path

## Problem Statement

The native-control fast path reduced MyLite-side direct-exec overhead for exact
`COMMIT`, `ROLLBACK`, and `SET autocommit = 0|1` statements, but MariaDB's
public `mysql_autocommit()` helper still calls `mysql_real_query()` with
`set autocommit=0` or `set autocommit=1`. WordPress PHPUnit repeats
`SET autocommit = 0` around many test cases after the connection is already in
non-autocommit mode, so the branch still pays a native SQL round trip for a
known no-op.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/libmysqld/libmysql.c:mysql_autocommit()` delegates to
  `mysql_real_query(mysql, "set autocommit=0", 16)` or
  `mysql_real_query(mysql, "set autocommit=1", 16)`.
- `mariadb/include/mysql.h` exposes `MYSQL::server_status`, and
  `mariadb/include/mysql_com.h` defines `SERVER_STATUS_AUTOCOMMIT`.
- `packages/libmylite/src/database.cc` already uses `MYSQL::server_status` for
  transaction state decisions after successful embedded statements.

## Scope And Non-Goals

In scope:

- Skip exact non-ownerless `SET autocommit = 0` when the embedded handle already
  reports autocommit off.
- Skip exact non-ownerless `SET autocommit = 1` when the embedded handle already
  reports autocommit on.
- Keep exact changed-state `SET autocommit`, `COMMIT`, and `ROLLBACK` delegated
  to MariaDB's public C API wrappers.
- Expose a diagnostic `native_control_autocommit_noops` profile counter through
  the PHP mysqli profile and WordPress timing-summary extraction.

Out of scope:

- Ownerless SQL execution, because ownerless statements still need statement
  locks, page-version refresh, dictionary coordination, and page publication.
- `START TRANSACTION`, `BEGIN`, `COMMIT AND CHAIN`, `ROLLBACK TO SAVEPOINT`,
  multi-assignment `SET`, or expression-valued `SET` statements.
- Reaching into server-internal `THD` transaction helpers.

## Design

The existing exact-statement classifier continues to accept only simple
`SET autocommit = 0|1` spellings with optional whitespace and one trailing
semicolon. In the non-ownerless direct-exec path:

1. Classify the statement.
2. If it is an autocommit statement and the requested state already matches
   `db.mysql.server_status & SERVER_STATUS_AUTOCOMMIT`, return success without
   calling `mysql_autocommit()`.
3. Otherwise call the existing MariaDB C API wrapper.
4. Preserve `changes=0`, `last_insert_id=mysql_insert_id()`, and the existing
   no-result accounting for native-control statements.

The profile bridge adds
`mylite_mysqli_profile_libmylite_exec_result_native_control_autocommit_noops`
so CI logs show how many native SQL round trips were removed.

## Compatibility Impact

This is behavior-preserving for the covered no-op statements: the requested
autocommit state already matches the embedded connection state, the statement
has no result set, and MyLite does not expose MariaDB warning-count changes
through its public API. Non-exact forms and changed-state forms still use
MariaDB SQL execution.

The public C API and PHP mysqli API do not change. The added profile key is a
diagnostic extension.

## Directory And Lifecycle Impact

No durable files, native storage formats, database-directory layout, or
ownerless runtime files change.

## Build, Size, License, And Dependencies

No dependency or license impact. The production library gains one small
connection-state check and one diagnostic counter.

## Verification Results

Focused production embedded and PHP build passed:

```text
cmake --build --preset php-embedded-prod --target mylite_embedded_exec_test mylite_mysqli_php_extension -j2
```

Focused behavior/profile tests passed:

```text
ctest --preset php-embedded-prod -R '^(libmylite\.embedded-exec|php-ext-mysqli-mylite\.profile)$' --output-on-failure
```

Verbose PHP profile evidence showed the new counter active in the small profile
test:

```text
mylite_mysqli_profile_libmylite_exec_result_native_control_calls=5
mylite_mysqli_profile_libmylite_exec_result_native_control_ms_total=6.183
mylite_mysqli_profile_libmylite_exec_result_native_control_errors=0
mylite_mysqli_profile_libmylite_exec_result_native_control_autocommit_noops=2
```

The WordPress production PHP build was refreshed before timing:

```text
MYLITE_WORDPRESS_REF=6ddfc9d9b532c6e95c1266165149815895e2eb56 \
MYLITE_WORDPRESS_PHASE=build-php \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_BUILD_TARGETS='mylite_mysqli_php_extension' \
tools/wordpress-phpunit-mysqli-mylite
```

A focused profiled production WordPress `^Tests_DB` run passed 651 tests with
3 skips:

```text
MYLITE_WORDPRESS_REF=6ddfc9d9b532c6e95c1266165149815895e2eb56 \
MYLITE_WORDPRESS_PHASE=phpunit \
MYLITE_WORDPRESS_SKIP_DOCKER_BUILD=1 \
MYLITE_WORDPRESS_CMAKE_BUILD_DIR=build/wordpress-php-embedded-prod \
MYLITE_WORDPRESS_CMAKE_BUILD_TYPE=Release \
MYLITE_WORDPRESS_REQUIRE_RELEASE_BUILD=1 \
MYLITE_WORDPRESS_REQUIRE_EXTERNAL_DB_DIR=1 \
MYLITE_WORDPRESS_PHPUNIT_NO_LOGGING=1 \
MYLITE_WORDPRESS_PHPUNIT_PROFILE_MYSQLI=1 \
MYLITE_WORDPRESS_PHPUNIT_KEEPALIVE=1 \
MYLITE_WORDPRESS_TIMING_LABEL=manual-autocommit-noop \
MYLITE_WORDPRESS_TIMING_SUMMARY_PATH=build/manual-wordpress-autocommit-noop-summary/timing-summary.md \
tools/wordpress-phpunit-mysqli-mylite --filter '^Tests_DB'
```

The main PHPUnit process reported:

```text
wordpress_phpunit_reported_seconds=7.798
wordpress_phpunit_shell_real_seconds=16.907
mylite_mysqli_profile_query_calls=4010
mylite_mysqli_profile_query_ms_total=5207.110
mylite_mysqli_profile_query_verb_set_calls=761
mylite_mysqli_profile_query_verb_set_ms_total=162.971
mylite_mysqli_profile_query_verb_transaction_calls=1310
mylite_mysqli_profile_query_verb_transaction_ms_total=1541.708
mylite_mysqli_profile_libmylite_exec_result_native_control_calls=1310
mylite_mysqli_profile_libmylite_exec_result_native_control_ms_total=742.014
mylite_mysqli_profile_libmylite_exec_result_native_control_errors=0
mylite_mysqli_profile_libmylite_exec_result_native_control_autocommit_noops=644
```

The timing summary file also recorded
`mylite_mysqli_profile_aggregate_libmylite_exec_result_native_control_autocommit_noops=644`
for the same `manual-autocommit-noop` run.

The earlier native-control fast-path sample recorded
`mylite_mysqli_profile_libmylite_exec_result_native_control_ms_total=1404.955`
for the same `1310` native-control call volume. The new run therefore removes
644 repeated autocommit SQL round trips from the focused WordPress `Tests_DB`
path and leaves changed-state/native transaction calls on the MariaDB path.

## Acceptance Criteria

- Exact repeated `SET autocommit = 0|1` no-ops succeed without calling
  `mysql_autocommit()`.
- Changed-state exact autocommit statements still call MariaDB's public C API
  wrapper.
- Extended transaction and `SET` forms still use the existing SQL path.
- The PHP mysqli profile and WordPress timing summary expose the autocommit
  no-op count.
- Focused embedded and PHP profile tests pass under production build presets.

## Risks And Follow-Up

- This is a narrow WordPress-shaped optimization. The follow-up
  `libmylite-start-transaction-fast-path` slice addresses exact non-ownerless
  `START TRANSACTION` with an embedded helper, but option-bearing transaction
  starts still require MariaDB SQL parsing.
