# libmylite Transaction End Profile

## Problem Statement

Production WordPress PHPUnit profiling showed transaction-control statements as
a hot bucket after the native-control and autocommit no-op work. The previous
slice exposed exact `START TRANSACTION` counts, but exact transaction ends were
still grouped only in the aggregate native-control timer. That made it hard to
separate start, commit, and rollback volume before choosing the next
optimization.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/libmysqld/libmysql.c:mysql_commit()` and
  `mysql_rollback()` delegate to `mysql_real_query("commit")` and
  `mysql_real_query("rollback")`.
- `mariadb/sql/sql_parse.cc` handles `SQLCOM_COMMIT` and `SQLCOM_ROLLBACK`
  with extra session semantics: `lex->tx_chain`, `lex->tx_release`, and
  `thd->variables.completion_type` decide whether the statement starts a
  chained transaction or releases the connection after the transaction ends.
- `mariadb/sql/transaction.cc:trans_commit()` and `trans_rollback()` perform
  the storage-engine transaction end and reset transaction bookkeeping, but the
  SQL command layer also releases transactional locks, applies chain/release
  policy, resets one-shot transaction characteristics, and sends the embedded
  OK/error result.
- A prototype MyLite embedded helper for exact default `COMMIT`/`ROLLBACK`
  preserved the no-chain source shape, but local performance evidence was
  negative, so it was rejected rather than committed.

## Scope And Non-Goals

In scope:

- Count exact non-ownerless direct `COMMIT` and full `ROLLBACK` statements in
  the existing native-control profile path.
- Surface those counters through the PHP mysqli profile and WordPress timing
  summary.
- Keep the PHP profile smoke covering both commit and rollback counters.
- Record why the direct embedded transaction-end helper was rejected.

Out of scope:

- Changing transaction-end execution semantics.
- Fast-pathing `COMMIT`, `ROLLBACK`, `COMMIT AND CHAIN`, `ROLLBACK AND CHAIN`,
  `RELEASE`, XA, savepoint rollback, or multi-statement SQL.
- Changing ownerless SQL routing, native storage, recovery, or directory layout.

## Design

Keep the existing classifier and execution behavior: exact non-ownerless
`COMMIT` continues to call MariaDB's `mysql_commit()`, and exact full
`ROLLBACK` continues to call `mysql_rollback()`. Add two diagnostic counters:

- `libmylite_exec_result_native_control_commit_calls`
- `libmylite_exec_result_native_control_rollback_calls`

The counters are incremented next to the existing aggregate
`libmylite_exec_result_native_control_calls` and
`libmylite_exec_result_native_control_start_transaction_calls` counters. They
do not affect SQL execution, native storage, public APIs, or ownerless routing.

The rejected native helper remains documented as a design result. Exact
transaction-end fast paths need a stronger performance proof than simple parser
bypass, because the isolated PHP sample showed parser-owned spellings were not
slower than the helper.

## Compatibility Impact

No SQL behavior changes. MariaDB remains authoritative for transaction-end
semantics, including `completion_type=CHAIN`, `completion_type=RELEASE`, and
explicit chain/release syntax.

## Directory And Lifecycle Impact

No durable files, native storage formats, database-directory layout, ownerless
runtime files, or page-version WAL records change.

## Build, Size, License, And Dependencies

No dependency, license, or MariaDB embedded delta remains in the accepted slice.
The first-party changes are limited to diagnostics, tests, and docs.

## Verification Results

Focused production embedded/PHP target build passed:

```text
cmake --build --preset php-embedded-prod --target mylite_embedded_exec_test mylite_mysqli_php_extension -j2
```

Focused behavior/profile tests passed:

```text
ctest --preset php-embedded-prod -R '^(libmylite\.embedded-exec|php-ext-mysqli-mylite\.profile)$' --output-on-failure
```

The verbose PHP profile smoke exposed the new counters:

```text
mylite_mysqli_profile_libmylite_exec_result_native_control_start_transaction_calls=2
mylite_mysqli_profile_libmylite_exec_result_native_control_commit_calls=1
mylite_mysqli_profile_libmylite_exec_result_native_control_rollback_calls=1
```

The accepted focused production WordPress `^Tests_DB` profile rerun passed 651
tests with 3 skips and exposed the production transaction-end split without
changing execution:

```text
wordpress_phpunit_reported_seconds=8.314
mylite_mysqli_profile_query_transaction_start_ms_total=828.318
mylite_mysqli_profile_query_transaction_end_ms_total=810.402
mylite_mysqli_profile_libmylite_exec_result_native_control_ms_total=1627.821
mylite_mysqli_profile_libmylite_exec_result_native_control_errors=0
mylite_mysqli_profile_libmylite_exec_result_native_control_start_transaction_calls=651
mylite_mysqli_profile_libmylite_exec_result_native_control_commit_calls=8
mylite_mysqli_profile_libmylite_exec_result_native_control_rollback_calls=651
```

The rejected helper prototype passed focused tests but regressed the focused
production WordPress `^Tests_DB` timing sample:

```text
wordpress_phpunit_reported_seconds=8.744
mylite_mysqli_profile_query_transaction_start_ms_total=899.176
mylite_mysqli_profile_query_transaction_end_ms_total=866.008
mylite_mysqli_profile_libmylite_exec_result_native_control_ms_total=1756.359
mylite_mysqli_profile_libmylite_exec_result_native_control_commit_calls=8
mylite_mysqli_profile_libmylite_exec_result_native_control_rollback_calls=651
```

The same prototype's local parser-control micro-benchmark did not support
landing it:

```text
native_start_native_rollback loops=800 total_ms=3219.103 per_pair_ms=4.024
parser_begin_parser_rollback_work loops=800 total_ms=3181.257 per_pair_ms=3.977
parser_begin_native_rollback loops=800 total_ms=4008.347 per_pair_ms=5.010
native_start_native_commit loops=800 total_ms=3501.883 per_pair_ms=4.377
parser_begin_parser_commit_work loops=800 total_ms=3354.140 per_pair_ms=4.193
parser_begin_native_commit loops=800 total_ms=3949.572 per_pair_ms=4.937
```

## Acceptance Criteria

- Exact non-ownerless direct `COMMIT` and full `ROLLBACK` profile counters are
  visible in C and PHP profile tests.
- WordPress timing summaries extract commit and rollback native-control
  counters.
- Transaction-end execution remains on MariaDB's existing public wrappers until
  a future design proves a non-regressing replacement.
