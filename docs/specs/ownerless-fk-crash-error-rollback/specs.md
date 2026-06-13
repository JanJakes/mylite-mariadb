# Ownerless FK Crash Fast-Path Error Rollback

## Problem Statement

The `dictionary-foreign-key-crash` hook selector recovers a killed
`ALTER TABLE ... ADD CONSTRAINT ... FOREIGN KEY` writer, then verifies that a
missing-parent child insert fails with MariaDB errno 1452 before a valid child
insert succeeds. After the transaction-end lock-grant slice, the selector
reached this FK case and exposed an ownerless-only failed-statement leak: the
orphan insert returned errno 1452, but primary key `2` remained visible in the
same ownerless handle before `COMMIT`, so the following valid insert failed
with duplicate-key errno 1062.

The same recovered files passed when the rejected insert was run through
ordinary native exclusive MyLite, so the recovered FK metadata and native
InnoDB enforcement were valid. The bug is in the ownerless fast-path statement
visibility around the post-recovery insert.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0mysql.cc:629` implements
  `row_mysql_handle_errors()`. `DB_NO_REFERENCED_ROW` and
  `DB_ROW_IS_REFERENCED` use the rollback-to-savepoint path when a savepoint is
  supplied.
- `mariadb/sql/transaction.cc:545` implements `trans_rollback_stmt()`, which
  rolls back the statement transaction through `ha_rollback_trans(thd, FALSE)`
  and resets statement transaction state.
- `mariadb/sql/sql_insert.cc` routes insert completion through the server
  statement/autocommit rollback machinery after handler errors.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` publishes ownerless page images
  directly from mini-transactions when the current SQL statement enables the
  visible fast path. That is safe only when the statement later succeeds.
- `packages/libmylite/src/database.cc` previously enabled the visible fast path
  for literal `INSERT ... VALUES` statements without checking whether the
  target table had FK constraints. An FK child insert can modify the child page
  before MariaDB reports `DB_NO_REFERENCED_ROW`, so the fast path could publish
  a rejected-row image before SQL success was known.

## Design

Split the literal-insert fast-path predicate into two parts:

1. Keep the existing syntactic fast-path check for simple
   `INSERT ... VALUES`.
2. Resolve the insert target table from the statement tokens.
3. Query `information_schema.referential_constraints` for the normalized target
   schema/table. If the target table owns FK constraints, disable the visible
   fast path so page images are published by the normal transaction completion
   path only after SQL success.
4. If the target table cannot be parsed or FK metadata cannot be read, disable
   the fast path conservatively.
5. Preserve the failed-statement diagnostic, run the existing internal rollback
   helper for ownerless implicit failures, close the current read view, refresh
   the current-read buffer boundary, and restore the original diagnostics.

Explicit multi-statement transactions keep MariaDB-compatible error handling;
the fast-path gate is specific to ownerless literal inserts whose target table
can reject rows through FK enforcement.

## Compatibility Impact

No SQL syntax, public C API, storage format, or directory layout changes. The
observable behavior aligns ownerless implicit failed DML with MariaDB
expectations: a failed missing-parent FK insert returns errno 1452 and does not
leave the rejected row pending for a later `COMMIT`.

The slice does not change explicit transaction semantics for duplicate-key,
foreign-key, check-constraint, or other statement errors. Literal inserts into
tables without FK constraints keep the visible fast path.

## Directory And Lifecycle Impact

No new files or shared-memory segments are added. The fix runs only on an open
ownerless handle after a failed SQL statement and reuses existing rollback and
ownerless transaction cleanup.

## Native Storage Impact

Native InnoDB remains responsible for FK checks, savepoint rollback, and
transaction state. MyLite prevents FK-constrained literal inserts from using
the pre-success page-image fast path and adds conservative ownerless
current-read cleanup after implicit statement failures.

## Binary Size Impact

The change adds small token/metadata helpers and failed-query cleanup call
sites. No new dependency or build-profile change is introduced.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct `dictionary-foreign-key-crash`.
- Run direct `dictionary-foreign-key-drop-crash`.
- Run direct `crash-tail` or the focused hook crash aggregate around FK
  dictionary cases.
- Run the ownerless FK normal selector to ensure non-crash FK DDL remains
  unchanged.
- Run focused ownerless hook CTest coverage, format checks, and
  `git diff --check`.

## Acceptance Criteria

- After recovered ADD FOREIGN KEY crash, a missing-parent child insert returns
  errno 1452 and leaves no row with the rejected primary key before `COMMIT`.
- The following valid child insert succeeds and persists.
- Ownerless/native reopen before and after forced `.shm` rebuild observe the
  same recovered FK metadata and rows.
- Non-crash ownerless FK DDL coverage still passes.
- Explicit transaction error semantics are not broadened by the helper.

## Verification Results

- `tools/mariadb-embedded-build build`
- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test`
- `timeout 240s build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-foreign-key-crash`
- `timeout 240s build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test foreign-key-ddl`
- `timeout 240s build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-foreign-key-drop-crash`
- `timeout 900s build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test crash-tail`
- `ctest --preset ownerless-test-hooks -R 'libmylite\.(ownerless-single-owner-multi-row-insert-visible-fast-path|embedded-ownerless-innodb-lock-hooks|ownerless-primitives)$' --output-on-failure`
- `timeout 900s build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test fk-graph-stress`
- `cmake --build --preset ownerless-test-hooks --target format-check`
- `git diff --check`

## Risks And Open Questions

- This slice fixes an ownerless autocommit failed-statement cleanup bug. It does
  not claim exhaustive FK DDL crash coverage, randomized FK graph crash fuzzing,
  or full external MariaDB/RQG stress.
- A future tighter integration could call MariaDB's statement rollback helper
  directly from the embedded boundary, but this slice avoids expanding the
  MyLite dependency on SQL-layer internals.
