# Prepared DML Reset Fast Path

## Problem

The production embedded performance probe showed that ownerless prepared
autocommit inserts were paying a MariaDB statement reset after every successful
no-result `INSERT`. In the reduced stats-enabled sample before this slice,
500 ownerless autocommit inserts spent `120.489 ms` in
`mylite_reset()`, with `120.397 ms` inside `mysql_stmt_reset()`. That was about
`0.241 ms` per insert and was unrelated to native page publication, redo,
checkpoint proof, or PHP/PHPUnit startup.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/libmariadb/libmariadb/mariadb_stmt.c:mysql_stmt_execute()` accepts
  a statement in `MYSQL_STMT_PREPARED` state and, when a previous execution has
  a pending unbuffered result without stored data, flushes it before setting
  the statement back to prepared state.
- `mysql_stmt_internal_reset()` and `mysql_stmt_reset()` perform broader work:
  clearing errors, freeing or draining result buffers, resetting long-data
  flags, and optionally sending `COM_STMT_RESET`.
- MyLite has no public long-data streaming API. It copies bound text/blob bytes
  before execution and rejects rebinding while a statement is still marked
  executed.
- MyLite keeps result metadata in `mylite_stmt::metadata` and
  `mylite_stmt::columns`; existing prepared `SELECT` tests verify that
  metadata remains available after `MYLITE_DONE` until reset.

## Design

Keep `mylite_reset()` semantically the same for callers, but avoid the
server-side MariaDB reset only when all of the following are true before result
cleanup:

- the statement has successfully executed;
- MyLite has no active result set;
- MyLite has no current row;
- no result metadata is present;
- the result-column cache is empty.

This covers completed no-result statements such as ordinary and ownerless DML.
Failed statements, unexecuted statements, result-bearing statements, and any
statement that still has metadata continue to call `mysql_stmt_reset()`.

`mylite_reset()` still clears MyLite result state, ownerless page visibility,
and the executed flag before returning `MYLITE_OK`. The fast path is internal:
the C API does not expose whether a server-side reset was necessary.

## Compatibility Impact

Prepared DML remains reusable after `mylite_reset()`. Bindings may still be
changed only before first execution or after successful reset. Prepared
`SELECT` metadata and result-drain semantics stay on the MariaDB reset path.
Failed statements also stay on the MariaDB reset path so MariaDB error/reset
behavior remains the authority for those cases.

## Native Storage And Directory Impact

No native storage format, ownerless WAL format, redo/checkpoint policy, or
directory lifecycle changes. The slice removes a client-library reset round
trip for completed no-result statements; it does not reduce ownerless
page-version records or native history proof requirements.

## Build And Performance Impact

The fast path removes the hot `mysql_stmt_reset()` interval for completed DML.
The post-change reduced stats-enabled production sample with 500 ownerless
autocommit inserts reported:

- `mylite_perf_ownerless_insert_autocommit_prepared_reset_calls=500`;
- `mylite_perf_ownerless_insert_autocommit_prepared_reset_total_ms=0.064`;
- `mylite_perf_ownerless_insert_autocommit_prepared_reset_mysql_ms=0.000`.

The companion stats-off production sample with 1000 inserts reported
ownerless autocommit at `1520.62 ops/s` versus ordinary autocommit at
`3756.37 ops/s`, and ownerless transactional inserts at `1567.77 ops/s`
versus ordinary transactional inserts at `4039.43 ops/s`. That improves the
previous ownerless stats-off sample (`1052.43 ops/s` autocommit,
`869.25 ops/s` transactional), but it still leaves the larger native
ownerless costs in page-log append, commit-MTR page publication,
write-history, and row-level MTR commit.

## Test And Verification Plan

- Build `mylite_embedded_prepared_statement_test` and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-prepared-statement$' --output-on-failure`.
- Run a reduced stats-enabled production embedded performance probe and
  confirm completed ownerless prepared DML resets no longer spend time in
  `mysql_stmt_reset()`.
- Run a stats-off production embedded performance probe for the throughput
  signal.
- Run focused ownerless primitive coverage under `php-embedded-prod`.
- Run the CI production-build audit.
- Run format, clang-tidy, and `git diff --check`.

## Acceptance Criteria

- Prepared DML reset/re-execution works in ordinary and ownerless handles.
- Prepared result statements retain their metadata behavior and stay on the
  conservative reset path.
- Production perf output shows ownerless prepared-DML reset `mysql` time at
  zero for the reduced insert sample.
- Documentation records that this is a prepared-statement fast path, not a
  native ownerless storage completion claim.

## Verification Results

Local verification on 2026-06-11 used the production `php-embedded-prod`
artifacts:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_prepared_statement_test mylite_embedded_performance_probe
  -j 4`: passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-prepared-statement$' --output-on-failure`: passed,
  including ownerless prepared DML reset/re-execution and duplicate-key
  failure recovery.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.(embedded-prepared-statement|ownerless-primitives)$'
  --output-on-failure`: passed, 2/2 tests.
- Reduced stats-enabled production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=2`,
  `MYLITE_PERF_SELECT_ITERATIONS=200`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`:
  passed and reported 500 ownerless autocommit resets with `0.064 ms` total
  reset time and `0.000 ms` inside `mysql_stmt_reset()`.
- Stats-off production probe with 1000 inserts passed and reported ownerless
  autocommit at `1520.62 ops/s`, ownerless transactional inserts at
  `1567.77 ops/s`, ordinary autocommit at `3756.37 ops/s`, and ordinary
  transactional inserts at `4039.43 ops/s`.
- `tools/check-ci-production-builds`: passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure`: passed.
- `tools/require-cmake-release-build build/prod build/php-embedded-prod
  build/wordpress-php-embedded-prod`: passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded
  build/wordpress-mariadb-embedded`: passed.
- `cmake --build --preset format-check-prod`: passed with clang-format 18.
- `cmake --build --preset tidy-prod`: passed.
- `git diff --check`: passed.
