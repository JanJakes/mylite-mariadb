# libmylite Start Transaction Fast Path

## Problem Statement

After the native-control and autocommit no-op fast paths, profiled production
WordPress PHPUnit still spends visible time in repeated exact
`START TRANSACTION` statements. MariaDB does not expose a public embedded C API
equivalent to `START TRANSACTION`, so the previous slice left those starts on
the full `mysql_query()` SQL parse path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` handles `SQLCOM_BEGIN` by calling
  `trans_begin(thd, lex->start_transaction_opt)`, releasing transactional locks
  on failure, and calling `my_ok(thd)` on success.
- `mariadb/sql/transaction.cc:trans_begin()` commits or clears any existing
  multi-statement transaction state, resets one-shot characteristics, applies
  `READ ONLY`, `READ WRITE`, and consistent-snapshot flags when present, sets
  `OPTION_BEGIN`, and updates `SERVER_STATUS_IN_TRANS`.
- `mariadb/libmysqld/lib_sql.cc:emb_advanced_command()` owns embedded command
  setup for `MYSQL`: killed/reconnect handling, command-order checks, clearing
  old results/errors, `THD::store_globals()`, and old-query cleanup.
- `mariadb/sql/protocol.cc:Protocol::end_statement()` turns the THD diagnostics
  area into an OK or error packet, and
  `mariadb/libmysqld/lib_sql.cc:emb_read_query_result()` copies that packet back
  into `MYSQL::server_status`, warning count, affected rows, insert id, and net
  diagnostics.

## Scope And Non-Goals

In scope:

- Fast-path exact non-ownerless direct `START TRANSACTION` statements with an
  optional trailing semicolon.
- Keep `START TRANSACTION READ ONLY`, `START TRANSACTION READ WRITE`,
  `START TRANSACTION WITH CONSISTENT SNAPSHOT`, `BEGIN`, and multi-statement SQL
  on MariaDB's parser path.
- Keep ownerless SQL on the existing statement-lock, page-version refresh, and
  publication path.
- Expose a diagnostic
  `libmylite_exec_result_native_control_start_transaction_calls` profile counter
  through the PHP mysqli profile and WordPress timing summary.

Out of scope:

- A public MyLite or MariaDB API for arbitrary transaction-start options.
- Replacing MariaDB `COMMIT`, `ROLLBACK`, or `mysql_autocommit()` internals.
- Any native storage, directory layout, ownerless lock, or recovery change.

## Design

Add a MyLite-owned embedded helper inside `mariadb/libmysqld/lib_sql.cc` so it can
use MariaDB's internal `THD` and transaction helpers without exposing them
through the public MyLite API. The helper mirrors the relevant no-result
embedded command lifecycle:

1. Handle killed/reconnect cases and command-order validation the same way as
   `emb_advanced_command()`.
2. Clear old embedded result data, old query metadata, and connection errors.
3. Set query time/id on the THD without initializing parser state.
4. Call `trans_begin(thd, 0)` to match exact `START TRANSACTION` with no parser
   options.
5. On failure, release transactional locks as `SQLCOM_BEGIN` does; on success,
   call `my_ok(thd)`.
6. Finalize status through `thd->update_server_status()` and
   `thd->protocol->end_statement()`.
7. Read the embedded OK/error dataset back into `MYSQL` with
   `emb_read_query_result()`.

The libmylite exact-statement classifier adds `START TRANSACTION` only when the
token stream is exactly `START`, `TRANSACTION`, and an optional trailing
semicolon. The existing non-ownerless native-control path then calls the helper,
updates the no-result counters, and preserves `changes=0` plus the current
insert id behavior.

## Compatibility Impact

Exact `START TRANSACTION` keeps MariaDB transaction semantics because it calls
the same `trans_begin(thd, 0)` function reached by `SQLCOM_BEGIN`, then uses the
same embedded protocol machinery for OK/error state. Option-bearing transaction
starts still require parsing because they map to `lex->start_transaction_opt`.

The public C API, PHP mysqli API, and SQL surface do not change. The added
profile key is diagnostic-only.

## Directory And Lifecycle Impact

No durable files, native storage formats, database-directory layout, ownerless
runtime files, or page-version WAL records change. Ownerless handles do not use
this helper.

## Build, Size, License, And Dependencies

No dependency or license impact. The MariaDB embedded delta is one
MyLite-prefixed helper in an existing upstream-derived file plus a small
classifier branch in first-party libmylite code.

## Verification Results

Focused production embedded and PHP build passed after rebuilding the MariaDB
embedded archive:

```text
tools/mariadb-embedded-build build
cmake --build --preset php-embedded-prod --target mylite_embedded_exec_test mylite_mysqli_php_extension -j2
```

Focused behavior/profile tests passed:

```text
ctest --preset php-embedded-prod -R '^(libmylite\.embedded-exec|php-ext-mysqli-mylite\.profile)$' --output-on-failure
```

Verbose PHP profile evidence showed the new counter active in the small profile
test:

```text
mylite_mysqli_profile_libmylite_exec_result_native_control_calls=6
mylite_mysqli_profile_libmylite_exec_result_native_control_errors=0
mylite_mysqli_profile_libmylite_exec_result_native_control_autocommit_noops=2
mylite_mysqli_profile_libmylite_exec_result_native_control_start_transaction_calls=1
```

The focused PHP micro-benchmark compared exact `START TRANSACTION` against
parser-routed `BEGIN`. The reduced helper was roughly neutral with run-to-run
noise:

```text
start loops=500 total_ms=1733.368 per_pair_ms=3.467
begin loops=500 total_ms=1783.963 per_pair_ms=3.568
start2 loops=500 total_ms=1754.639 per_pair_ms=3.509
begin2 loops=500 total_ms=1703.949 per_pair_ms=3.408
start loops=1000 total_ms=1767.534 per_start_ms=1.768
begin loops=1000 total_ms=1705.947 per_start_ms=1.706
start2 loops=1000 total_ms=1691.573 per_start_ms=1.692
begin2 loops=1000 total_ms=1687.753 per_start_ms=1.688
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
MYLITE_WORDPRESS_TIMING_LABEL=manual-start-transaction-minimal \
MYLITE_WORDPRESS_TIMING_SUMMARY_PATH=build/manual-wordpress-start-transaction-minimal-summary/timing-summary.md \
tools/wordpress-phpunit-mysqli-mylite --filter '^Tests_DB'
```

The main PHPUnit process reported:

```text
wordpress_phpunit_reported_seconds=8.252
wordpress_phpunit_shell_real_seconds=16.360
mylite_mysqli_profile_query_calls=4010
mylite_mysqli_profile_query_ms_total=5839.624
mylite_mysqli_profile_query_verb_transaction_calls=1310
mylite_mysqli_profile_query_verb_transaction_ms_total=1432.767
mylite_mysqli_profile_query_transaction_start_calls=651
mylite_mysqli_profile_query_transaction_start_ms_total=725.444
mylite_mysqli_profile_query_transaction_end_calls=659
mylite_mysqli_profile_query_transaction_end_ms_total=707.323
mylite_mysqli_profile_libmylite_exec_result_native_control_calls=1961
mylite_mysqli_profile_libmylite_exec_result_native_control_ms_total=1426.880
mylite_mysqli_profile_libmylite_exec_result_native_control_errors=0
mylite_mysqli_profile_libmylite_exec_result_native_control_autocommit_noops=644
mylite_mysqli_profile_libmylite_exec_result_native_control_start_transaction_calls=651
```

The timing summary also recorded
`mylite_mysqli_profile_aggregate_libmylite_exec_result_native_control_start_transaction_calls=651`.

## Test And Verification Plan

- Extend `libmylite.embedded-exec` to prove exact `START TRANSACTION` increments
  the new native-control start counter, preserves rollback/commit behavior, and
  leaves `START TRANSACTION READ ONLY` on the SQL path.
- Extend `php-ext-mysqli-mylite.profile` to require the new profile counter.
- Rebuild production embedded/PHP targets and run the focused C/PHP tests.
- Run a focused profiled production WordPress `^Tests_DB` sample to confirm the
  counter is visible and transaction-start timing moves.
- Run shell syntax, production-build guard, format-check, and `git diff --check`.

## Acceptance Criteria

- Exact non-ownerless `START TRANSACTION` succeeds through the native-control
  path and updates `MYSQL` status/diagnostics through embedded OK/error
  handling.
- Option-bearing transaction-start statements do not use the fast path.
- Ownerless execution does not use the helper.
- Focused production embedded and PHP profile tests pass.
- WordPress timing summaries expose
  `mylite_mysqli_profile_aggregate_libmylite_exec_result_native_control_start_transaction_calls`.

## Risks And Follow-Up

- This helper intentionally mirrors only the embedded no-result lifecycle needed
  for exact `START TRANSACTION`. Broader transaction option support would need a
  separate design because parser flags matter.
- The next performance target after this slice is likely transaction end/native
  commit cost or ownerless page-publication cost, depending on the post-change
  WordPress timing sample.
