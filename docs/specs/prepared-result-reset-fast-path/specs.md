# Prepared Result Reset Fast Path

## Problem

Production WordPress mysqli timing shows repeated result-query execution is
dominated by prepared-statement lifecycle cost, not PHP result materialization.
A focused `MYLITE_MYSQLI_PROFILE=1` loop over cached `mysqli_query('SELECT 1')`
reported one prepare, 999 cache hits, and about 1.6 ms per query in cached
statement reset plus about 1.6 ms per query in result stepping. The adapter uses
`mylite_reset()` before re-executing the cached statement, and `mylite_reset()`
currently sends `mysql_stmt_reset()` for completed result statements.

## Source Findings

- Target base: MariaDB 11.8.6, initial import
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `packages/libmylite/src/database.cc` uses `mysql_stmt_execute()`,
  `mysql_stmt_fetch()`, `mysql_stmt_next_result()`, `mysql_stmt_free_result()`,
  and `mysql_stmt_reset()` behind the public `mylite_stmt` API.
- `mariadb/tests/mysql_client_test.c` re-executes a prepared statement after
  fetching `MYSQL_NO_DATA` without an intervening `mysql_stmt_reset()`, proving
  MariaDB supports repeated execution after a result is fully consumed.
- `mariadb/libmariadb/unittest/libmariadb/ps_bugs.c` also covers repeated
  execution after `mysql_stmt_free_result()` and after empty result sets.
- `mariadb/sql/sql_prepare.cc::mysqld_stmt_reset()` closes any open cursor,
  clears long-data parameters, and returns the statement to
  `Query_arena::STMT_PREPARED`. MyLite must keep that conservative reset for
  partial results and error paths.

## Design

Treat a MyLite statement with `executed == true`, `has_result == false`, and
`has_row == false` as fully drained. In that state, `mylite_reset()` clears
ownerless visibility state and marks the statement unexecuted without sending
`mysql_stmt_reset()`. Result metadata and bind buffers are retained so the next
execution can reuse the existing `mysql_stmt_bind_result()` setup for the same
prepared SQL.

Partial result sets, current rows, failed statements, and statements with
unconsumed server-side cursor state still release client-side result state and
use `mysql_stmt_reset()`. Completed no-result DML keeps the existing fast path.

## Compatibility Impact

The public `mylite_reset()` contract is unchanged: callers can rebind and
re-execute after reset. The implementation now relies on MariaDB's supported
prepared-statement behavior that `mysql_stmt_execute()` may be called again
after results have been fully consumed and result binds may be reused for a
prepared statement's stable result metadata.

## Lifecycle And Storage Impact

No durable layout, file ownership, or database-directory state changes. The
slice removes an avoidable prepared-statement round trip from fully drained
statements in both ordinary and ownerless handles.

## Test Plan

- Extend `mylite_embedded_prepared_statement_test` to re-execute a completed
  result statement after `mylite_reset()`.
- Add partial-result coverage proving reset remains valid before all rows are
  fetched.
- Run focused PHP mysqli profile and WordPress performance probes on production
  builds.
- Run focused embedded/PHP CTest, format, diff, and CI production-build audit.

## Acceptance Criteria

- Completed result statements can be reset and re-executed with current data
  while reusing result metadata and bind buffers.
- Partial result reset still discards the active result and allows
  re-execution.
- Cached mysqli result-query reset time drops materially in the focused
  `SELECT 1` profile without weakening metadata behavior.
- Production-build CI guards remain green.

## Verification Evidence

- A production focused profile over 1000 cached `mysqli_query('SELECT 1')`
  calls reported `query_cache_hits=999`, `query_prepare_calls=1`,
  `query_cache_lookup_ms_total=0.371`, and
  `php_select1_ops_per_second=789.52`.
- A CI-shaped production WordPress performance probe reported
  `select1_ops_per_second=829.69`; the previous production probe before this
  fast path reported about `383.57`.
- A focused production `Tests_DB` run reported
  `wordpress_phpunit_reported_seconds=16.442` and
  `wordpress_phpunit_shell_real_seconds=28.889`. The same profile still had
  only three main-process result-cache hits, so broader suite time remains
  dominated by varied-query prepare/execute work and harness overhead.

## Risks

The fast path assumes MyLite's `fetch_statement_row()` has consumed every
server-side result set before setting `has_result` false. The conservative
predicate keeps partial or active-result statements on the MariaDB reset path.
