# Ownerless Generated Column Blocked Function Upstream Matrix

## Problem

Ownerless generated-column policy already covers representative aggregate,
subquery, time-dependent, session-dependent, and nondeterministic function
classes. MariaDB's upstream virtual-column blocked-function suite also includes
crypto, statement-state, user/version, lock/wait, and UUID-short function
families. MyLite should keep the retained-function MariaDB validation failures
visible in ownerless mode and prove failed DDL leaves no native metadata side
effects.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/mysql-test/suite/vcol/t/vcol_blocked_sql_funcs_main.inc` is the
  upstream compatibility evidence for blocked generated-column SQL functions,
  including `AES_ENCRYPT()`, `FOUND_ROWS()`, `GET_LOCK()`,
  `LAST_INSERT_ID()`, `ROW_COUNT()`, `SLEEP()`, `USER()`, `VERSION()`, and
  `UUID_SHORT()`.
- MyLite's default embedded profile deliberately rejects or omits server
  utility functions such as `GET_LOCK()`, `SLEEP()`, and `UUID_SHORT()` before
  MariaDB generated-column validation. Those functions stay covered by the
  server-utility SQL policy instead of this errno-1901 matrix.
- `mariadb/sql/field.cc` validates generated-column expressions through
  `Item::check_vcol_func_processor()` and rejects disallowed functions with
  MariaDB errno 1901.
- `packages/libmylite/src/database.cc` lets ownerless generated-column DDL
  reach MariaDB validation, then relies on the ownerless dictionary boundary
  and native reopen checks to prove failed DDL does not leak metadata.

## Design

Extend the existing
`generated-column-blocked-function-policy` selector instead of adding a second
long-running generated-column test. Add rejected `CREATE TABLE` cases for
representative upstream families that remain in the embedded SQL profile:

- crypto: `AES_ENCRYPT()`,
- statement/session state: `FOUND_ROWS()`, `LAST_INSERT_ID()`, `ROW_COUNT()`,
- user/version: `USER()`, `VERSION()`.

Also add failed `ALTER TABLE ... ADD COLUMN` / `MODIFY COLUMN` cases for
`ROW_COUNT()`, `USER()`, and `VERSION()`. The selector verifies MariaDB errno
1901, absence of rejected tables and columns, preservation of the existing
generated-column table, and ownerless/native reopen before and after forced
`.shm` rebuild.

## Scope And Non-Goals

In scope:

- Focused ownerless SQL coverage for additional upstream blocked-function
  families.
- Documentation and compatibility-matrix updates.

Out of scope:

- Exhaustive replay of every upstream MTR case.
- Re-testing server utility functions that MyLite intentionally rejects before
  generated-column validation, including `GET_LOCK()`, `SLEEP()`, and
  `UUID_SHORT()`.
- Production SQL behavior changes.
- Stored-routine generated-column support.
- External MariaDB/RQG generated-column stress.

## Compatibility Impact

No SQL semantics change. This slice increases ownerless compatibility evidence
that MariaDB-generated errno 1901 remains stable for additional retained
upstream blocked-function families and failed DDL remains side-effect free.

## Directory And Lifecycle Impact

No directory layout changes. The test proves rejected generated-column DDL does
not create native table/column/index metadata and the surviving state remains
stable through ownerless reopen, native exclusive reopen, and forced shared
memory rebuild.

## Native Storage Impact

No native storage format changes. Failed generated-column DDL must not reach a
completed InnoDB metadata mutation.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice changes only test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `generated-column-blocked-function-policy` selector.
- Run adjacent generated-column policy selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run relevant ownerless SQL CTest shards if the selector index changes.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Additional retained upstream-family generated-column expressions fail with
  MariaDB errno 1901 under ownerless mode.
- Rejected create cases leave no rejected tables.
- Rejected alter cases leave no rejected columns and preserve existing
  generated-column data.
- Final state survives ownerless/native reopen before and after forced `.shm`
  rebuild.

## Risks And Follow-Up

- This remains representative coverage, not exhaustive upstream MTR replay.
- External generated-column oracle stress remains planned.
