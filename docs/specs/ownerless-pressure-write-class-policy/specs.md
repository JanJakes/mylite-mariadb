# Ownerless Pressure Write-Class Policy

## Problem

Ownerless active-reader pressure coverage proves the page-version WAL soft cap
blocks a direct `UPDATE` and a prepared `UPDATE` while a repeatable-read
snapshot pin retains WAL. It does not prove that the same gate is applied
before other representative SQL write classes reach MariaDB execution, nor that
non-mutating reads and transaction-control statements continue to work while
pressure is active.

That leaves a policy gap: the soft cap is meant to throttle ownerless mutations
under retained-WAL pressure, not just one DML spelling.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_INSERT`, `SQLCOM_UPDATE`,
  `SQLCOM_DELETE`, `SQLCOM_CREATE_TABLE`, `SQLCOM_ALTER_TABLE`, and
  `SQLCOM_DROP_TABLE` as data-changing commands.
- `mariadb/sql/sql_parse.cc` dispatches representative DML through
  `mysql_insert()`, `mysql_update()`, and delete execution paths before native
  engine mutation.
- `mariadb/sql/sql_parse.cc` dispatches representative DDL through
  `mysql_create_table()`, `mysql_alter_table()`, and `mysql_rm_table()`.
- MyLite applies `enforce_ownerless_page_log_limit_policy()` before statement
  execution. The policy uses the SQL write classifier, reads the ownerless
  pressure state, and returns `MYLITE_BUSY` before MariaDB executes a write
  when active snapshot pins retain WAL at or above the configured soft limit.

## Design

Add focused ownerless SQL coverage for a live repeatable-read snapshot pin:

1. Create durable baseline InnoDB tables and close cleanly.
2. Start a peer repeatable-read snapshot pin.
3. Commit one ownerless update so page-version WAL is retained by the pin.
4. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
5. Verify ordinary `SELECT`, `START TRANSACTION`, and `ROLLBACK` still work.
6. Verify representative `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE`,
   `ALTER TABLE`, and `DROP TABLE` statements return `MYLITE_BUSY` with the
   pressure-limit diagnostic and do not mutate table rows, table metadata, or
   schema objects.
7. Release the reader pin and prove the same handle can execute the previously
   throttled write classes, then verify final state through ownerless/native
   reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Direct statement coverage for representative DML and table DDL write classes.
- Non-mutating SQL under pressure.
- Final ownerless/native reopen checks after the pressure clears.

Out of scope:

- A background checkpoint scheduler.
- Changing the soft-cap policy from pre-execution throttling to hard byte-limit
  accounting.
- External MariaDB/RQG pressure stress.

## Compatibility Impact

SQL semantics are unchanged. The slice expands compatibility evidence for the
ownerless pressure policy by proving pressure throttling is class-based and
state-preserving across representative write statements.

## Directory And Lifecycle Impact

No directory layout changes. The test continues to use the existing
`concurrency/mylite-concurrency.wal`, `.ckpt`, and `.shm` lifecycle, including
forced `.shm` rebuild verification.

## Native Storage Impact

No native storage format changes. The blocked DML and DDL statements must not
reach native mutation paths while pressure is active; after pressure clears,
the same native DML/DDL paths remain usable.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds test code and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `active-reader-pressure-write-policy` selector in
  `embedded-dev`.
- Run the focused selector in `ownerless-test-hooks`.
- Run the embedded ownerless cross-process SQL CTest coverage.
- Run the hook ownerless cross-process SQL CTest coverage.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The pressure-limited handle can execute non-mutating read and transaction
  control statements.
- Representative `INSERT`, `UPDATE`, `DELETE`, `CREATE TABLE`, `ALTER TABLE`,
  and `DROP TABLE` statements return `MYLITE_BUSY` while active-reader WAL
  pressure is at the configured limit.
- Blocked writes leave row data, column metadata, created-table absence, and
  drop-target presence unchanged.
- After the reader releases, the same handle can perform the previously
  blocked write classes.
- Final ownerless and native exclusive reopen checks pass before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- The policy remains a pre-execution soft cap. A write can still grow WAL after
  passing the preflight check if pressure was below the limit at statement
  start.
- Background checkpoint scheduling and external randomized pressure stress
  remain planned.
