# Ownerless Table-Wait Replacement Copy Negative Proof

## Problem

SQL-level ownerless table-lock fault injection remains planned because explored
blocked DDL shapes stop before the native InnoDB table-wait callback. Recent
ownerless work added `CREATE OR REPLACE TABLE ... LIKE` and
`CREATE OR REPLACE TABLE ... AS SELECT` evidence in pressure and crash
selectors. The hook-only table-wait negative proof should include these
replacement-copy DDL spellings so the unsupported table-wait boundary remains
tracked for the same destructive replacement family.

This slice adds negative evidence only. It does not claim SQL-level table-lock
fault injection coverage.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc` implements `CREATE OR REPLACE TABLE`,
  `CREATE TABLE ... LIKE`, and `CREATE TABLE ... SELECT` replacement-copy
  paths.
- `mariadb/sql/sql_base.cc` acquires metadata locks before opening
  non-temporary tables for many DDL paths.
- `mariadb/storage/innobase/lock/lock0lock.cc` publishes native InnoDB
  table-lock waits through `lock_table_enqueue_waiting()`.
- `packages/libmylite/src/database.cc` arms the unsafe `table-lock-wait` fault
  only at the ownerless InnoDB table-wait callback. If a tested SQL shape
  reaches that callback, the hook child signals the test pipe and the selector
  fails.

## Design

Extend `test_ownerless_table_wait_sql_negative_proof()`:

1. Seed `LIKE` and CTAS source tables before the blocking transaction starts.
2. Add blocked `CREATE OR REPLACE TABLE app.ownerless_sql LIKE ...` and
   `CREATE OR REPLACE TABLE app.ownerless_sql ENGINE=InnoDB AS SELECT ...`
   cases to the existing test matrix.
3. Keep the pass condition unchanged: each child must return MariaDB lock wait
   timeout without signaling the `table-lock-wait` fault pipe.
4. Keep the final original-table metadata checks so a blocked replacement-copy
   attempt cannot leave copied metadata or rows behind.

## Scope And Non-Goals

In scope:

- Hook-only negative proof for replacement-copy DDL shapes.
- Documentation that keeps SQL-level table-lock fault injection marked planned.

Out of scope:

- Positive SQL-level table-lock fault injection.
- Enabling ownerless `LOCK TABLES`.
- Changing production lock, DDL, or recovery behavior.
- External randomized DDL/RQG stress.

## Compatibility Impact

No supported SQL behavior changes. The selector only documents that these
currently tested replacement-copy DDL shapes still stop before the ownerless
native table-wait callback while another ownerless process holds the target
table.

## Directory And Lifecycle Impact

No directory layout changes. The existing selector verifies ownerless reopen,
forced `.shm` rebuild, and native exclusive reopen after the blocked DDL matrix.

## Native Storage Impact

No native storage format changes. Blocked replacement-copy attempts must leave
the original target table's rows, index, collation, and row-format metadata
unchanged.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The change is hook-build test and docs only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test`.
- Run
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  table-lock-wait-negative-proof`.
- Run `ctest --preset ownerless-test-hooks -L compat.ownerless-negative-proof
  --output-on-failure`.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Both replacement-copy DDL cases return MariaDB lock wait timeout while the
  holder transaction is active.
- Neither case signals the `table-lock-wait` fault pipe.
- The original target table remains readable and unchanged through ownerless
  reopen, forced `.shm` rebuild, and native exclusive reopen.
- Docs continue to mark SQL-level table-lock fault injection as planned.

## Risks And Follow-Up

- A future MariaDB flow change could route one of these SQL shapes into the
  native table-wait callback. That would fail this negative proof and should
  trigger a positive SQL fault-injection slice instead of a broader negative
  claim.
