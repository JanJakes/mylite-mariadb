# Ownerless Sequence Prepare Policy

## Problem

Ownerless read/write mode deliberately rejects MariaDB sequence SQL because
sequences are table-backed and sequence value functions can mutate sequence
state. Existing coverage proved direct top-level sequence DDL/value rejection
and hidden `DEFAULT NEXTVAL()` execution rejection, including prepared inserts
that pass MyLite's top-level SQL policy and fail through the MariaDB sequence
function guard. It did not explicitly prove that prepared top-level sequence
value statements fail before prepared-statement allocation.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`.
- `packages/libmylite/src/database.cc:3627` calls
  `reject_unsupported_sql_policy()` before `mysql_stmt_init()` and
  `mysql_stmt_prepare()` in `prepare_impl()`.
- `packages/libmylite/src/database.cc:4055` rejects ownerless sequence DDL and
  sequence value syntax, including `NEXT VALUE FOR`, `PREVIOUS VALUE FOR`,
  `NEXTVAL()`, `LASTVAL()`, and `SETVAL()`.
- `mariadb/sql/item_func.cc:7189` and `mariadb/sql/item_func.cc:7287` reject
  sequence value execution while ownerless runtime hooks are installed. This is
  the hidden-expression guard for metadata-driven sequence use such as
  `DEFAULT NEXTVAL()`.

## Design

Keep sequence semantics unsupported in ownerless read/write mode. Add focused
SQL compatibility coverage proving prepared top-level sequence value statements
are rejected by the MyLite policy boundary before a `mylite_stmt` is allocated,
while retaining the existing hidden-expression coverage that fails at MariaDB
execution time.

No production behavior change is required for this slice.

## Impact

- MySQL/MariaDB compatibility: ordinary exclusive sequence behavior remains
  supported; ownerless sequence coordination remains unsupported and explicit.
- Database directory lifecycle: no new files or layout changes.
- Native storage: no sequence-table mutation is allowed under ownerless mode.
- Public API: no API changes.
- Binary size and dependencies: no new dependencies and no measurable size
  impact beyond test/doc text.

## Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` under `php-embedded-prod`.
- Run the focused direct ownerless SQL case
  `mylite_ownerless_cross_process_sql_test sql-case test_ownerless_rejects_sequence_sql`.
- Run format and whitespace checks.

## Acceptance Criteria

- Prepared ownerless `SELECT NEXT VALUE FOR`, `PREVIOUS VALUE FOR`,
  `NEXTVAL()`, `LASTVAL()`, and `SETVAL()` all fail with the existing sequence
  policy message before statement allocation.
- Existing direct sequence DDL/value rejection and hidden `DEFAULT NEXTVAL()`
  direct/prepared insert rejection continue to pass.
- Docs distinguish prepared top-level policy rejection from hidden sequence
  execution rejection.

## Risks

This slice does not design ownerless sequence coordination. Broader sequence
DDL, sequence-table locking, crash recovery, and external stress remain planned.
