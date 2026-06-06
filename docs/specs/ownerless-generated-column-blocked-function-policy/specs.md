# Ownerless Generated Column Blocked Function Policy

## Problem

Ownerless generated-column coverage verifies deterministic DDL, secondary
indexes, indexed expression replacement, generated-column primary-key rejection,
and representative `RAND()` nondeterministic rejection. The compatibility matrix
still marks broader blocked-function matrices as planned, so ownerless mode
lacks focused evidence that MariaDB's generated-column function validation
continues to reject representative impossible, time-dependent, session-dependent,
crypto, statement-state, user/version, and non-deterministic expressions without
leaving partial DDL state behind.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/field.cc:10814-10827` walks generated-column expressions with
  `Item::check_vcol_func_processor`, always rejects `VCOL_IMPOSSIBLE`, and
  additionally rejects `VCOL_NOT_STRICTLY_DETERMINISTIC` for stored generated
  columns.
- `mariadb/sql/field.h:564-573` defines the relevant flag classes:
  `VCOL_NON_DETERMINISTIC`, `VCOL_SESSION_FUNC`, `VCOL_TIME_FUNC`,
  `VCOL_IMPOSSIBLE`, and `VCOL_NOT_STRICTLY_DETERMINISTIC`.
- `mariadb/sql/field.cc:11344-11352`
  `Column_definition::check_vcol_for_key()` rejects key use for generated
  columns whose expression flags include `VCOL_NOT_STRICTLY_DETERMINISTIC`.
- `mariadb/sql/item_sum.cc:626-630` marks aggregate functions as
  `VCOL_IMPOSSIBLE`.
- `mariadb/sql/item_subselect.h:243-246` marks subqueries as
  `VCOL_IMPOSSIBLE`.
- `mariadb/sql/item_timefunc.h:628-633` marks current timestamp family functions
  as time-dependent generated-column functions.
- `mariadb/sql/item_func.h:1407-1425` marks `CONNECTION_ID()` as a
  session-dependent generated-column function.
- `mariadb/sql/item_strfunc.h:272-276` marks `DATABASE()` as a
  session-dependent generated-column function.
- `mariadb/mysql-test/suite/vcol/t/vcol_blocked_sql_funcs_main.inc` records
  MariaDB's upstream blocked-function expectations across impossible,
  time-dependent, session-dependent, aggregate, subquery, and nondeterministic
  function families, plus additional built-ins such as `AES_ENCRYPT()`,
  `FOUND_ROWS()`, `LAST_INSERT_ID()`, `ROW_COUNT()`, `USER()`, and
  `VERSION()`.

## Scope And Non-Goals

In scope:

- Add ownerless SQL coverage for representative rejected create-time generated
  expressions:
  - aggregate `SUM(...)`,
  - subquery `(SELECT ...)`,
  - stored `CURRENT_TIMESTAMP()`,
  - stored `DATABASE()`,
  - stored `UUID()`,
  - `AES_ENCRYPT()`,
  - `FOUND_ROWS()`,
  - `LAST_INSERT_ID()`,
  - `ROW_COUNT()`,
  - `USER()`,
  - `VERSION()`.
- Add ownerless ALTER coverage proving failed `ADD COLUMN` and `MODIFY COLUMN`
  attempts from the same function classes return MariaDB errno 1901 and preserve
  the previous table definition/data, including retained upstream
  statement-state and user/version functions.
- Add ownerless virtual-column index coverage proving virtual
  non-strictly-deterministic/session generated columns may be defined but remain
  non-indexable with MariaDB errno 1901.
- Verify absence of rejected tables, columns, and indexes through ownerless and
  native reopen before and after forced `.shm` rebuild.

Out of scope:

- Exhaustively replay every function in MariaDB's upstream blocked-function
  suite.
- Re-test upstream blocked-function cases for server utility functions that
  MyLite intentionally rejects before generated-column validation, including
  `GET_LOCK()`, `SLEEP()`, and `UUID_SHORT()`.
- Successful generated-column DDL crash injection.
- External MariaDB/RQG long-running generated-column oracle stress.
- SQL-level table-lock wait fault injection; prior investigation found the
  explored SQL shapes timed out before the ownerless table-wait callback.

## Design

Add a `generated-column-blocked-function-policy` selector to
`mylite_ownerless_cross_process_sql_test`.

The selector opens an ownerless read/write handle and verifies:

1. Representative rejected `CREATE TABLE` generated-column expressions fail with
   `ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED` / errno 1901 and leave no
   rejected tables in `information_schema.tables`.
2. A deterministic generated-column table remains intact after failed
   `ALTER TABLE ... ADD COLUMN` and `ALTER TABLE ... MODIFY COLUMN` attempts
   that use time-dependent, aggregate, nondeterministic, and subquery
   expressions.
3. A virtual generated-column table with `RAND()`, `CONNECTION_ID()`, and
   `DATABASE()` remains writable, while standalone and alter-time index attempts
   over those generated columns fail with errno 1901 and leave no index
   metadata.
4. Ownerless/native reopen checks, plus forced shared-memory rebuild, verify the
   same final table, column, index, and aggregate state.

## Compatibility Impact

This broadens ownerless generated-column policy evidence from one representative
`RAND()` path to representative MariaDB flag classes and additional retained
upstream built-ins. It does not claim complete coverage of every blocked
built-in function or every generated-column DDL option.

## Directory And Lifecycle Impact

No new durable files or layout changes. The test verifies that failed
generated-column DDL does not leave native metadata artifacts in `datadir/` and
that ordinary ownerless/native reopen plus forced `.shm` rebuild continue to
observe the same final state.

## Native Storage Impact

No native storage format changes. The selector exercises MariaDB's existing SQL
validation before unproven generated-column definitions reach InnoDB metadata.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `generated-column-blocked-function-policy` selector.
- Run adjacent generated-column selectors:
  `generated-column-nondeterministic-policy`,
  `generated-column-indexed-expression-policy`, `generated-column-index-ddl`,
  and `generated-column-primary-key-policy`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the embedded ownerless cross-process SQL label, ownerless stress,
  `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Representative impossible generated-column expressions fail with errno 1901
  and leave no rejected tables.
- Representative stored time/session/nondeterministic generated-column
  expressions plus retained crypto, statement-state, and user/version functions
  fail with errno 1901 and leave no rejected columns or expression replacements.
- Representative virtual non-strictly-deterministic/session generated columns
  stay non-indexed after failed standalone and alter-time index DDL.
- Final state survives ownerless/native reopen before and after forced `.shm`
  rebuild.

## Risks And Follow-Up

- The selector is a representative matrix, not an exhaustive copy of MariaDB's
  upstream blocked-function test.
- Failed generated-column DDL crash recovery is covered by
  `ownerless-generated-column-failed-ddl-crash`; successful generated-column
  DDL crash recovery remains planned.
- External MariaDB/RQG oracle execution remains environment-owned follow-up
  work.
