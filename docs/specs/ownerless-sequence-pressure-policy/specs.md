# Ownerless Sequence Pressure Policy

## Problem

The active-reader pressure limiter returns `MYLITE_BUSY` for supported
ownerless write classes when retained page-version WAL is at the configured
soft cap. Unsupported ownerless SQL surfaces must keep their explicit policy
diagnostics under the same pressure so CI and application logs distinguish
"try again later" from "unsupported in ownerless mode." Existing pressure-order
coverage included table-admin SQL, locked-table SQL, flush read-lock/export,
host-file import, events, tablespace detach/import, and storage-option DDL, but
not top-level sequence SQL.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`.
- `packages/libmylite/src/database.cc:3440` and
  `packages/libmylite/src/database.cc:3628` call
  `reject_unsupported_sql_policy()` before ownerless execution or prepared
  statement allocation.
- `packages/libmylite/src/database.cc:3471` applies the ownerless page-log
  pressure policy only after unsupported SQL policy rejection for direct
  execution.
- `packages/libmylite/src/database.cc:4055` rejects ownerless sequence DDL and
  top-level sequence value syntax.
- `mariadb/sql/item_func.cc:7189` and `mariadb/sql/item_func.cc:7287` remain
  the separate hidden sequence execution guard for metadata-driven sequence use.

## Design

Extend `test_ownerless_active_reader_pressure_limit_blocks_write_classes()` with
a sequence created under ordinary exclusive mode before ownerless pressure is
introduced. While a repeatable-read ownerless reader pins retained WAL at the
soft cap, assert that:

- `CREATE SEQUENCE`, `ALTER SEQUENCE`, and `DROP SEQUENCE` return the explicit
  ownerless sequence policy error;
- direct `SELECT NEXT VALUE FOR` and prepared `SELECT NEXTVAL()` return the same
  policy error before pressure handling or prepared-statement allocation;
- the attempted new sequence remains absent, the existing sequence remains
  present, and the table using `DEFAULT NEXTVAL()` remains unchanged.

The slice intentionally does not assert hidden `DEFAULT NEXTVAL()` insert
diagnostics under pressure. That statement shape is a normal `INSERT` until
MariaDB evaluates existing metadata, so the pressure limiter may correctly block
it as a supported write before the hidden sequence execution guard runs.

## Impact

- MySQL/MariaDB compatibility: ordinary exclusive sequence behavior remains
  supported; ownerless sequence coordination remains unsupported.
- Database directory lifecycle: no new layout or durable-file policy changes.
- Native storage: sequence-table state is not advanced by ownerless top-level
  sequence SQL under pressure.
- Public API: no API changes.
- Binary size and dependencies: no production code or dependency changes.

## Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` under `php-embedded-prod`.
- Run direct ownerless SQL case
  `mylite_ownerless_cross_process_sql_test sql-case test_ownerless_active_reader_pressure_limit_blocks_write_classes`.
- Run production format and whitespace checks.

## Acceptance Criteria

- Top-level sequence DDL/value SQL returns the sequence policy error, not
  `MYLITE_BUSY`, while retained WAL is at the configured pressure limit.
- Prepared top-level sequence value SQL fails before statement allocation under
  the same pressure.
- Existing sequence/default-table state remains unchanged through ownerless and
  native reopen before and after forced `.shm` rebuild.

## Risks

This slice does not implement ownerless sequence coordination, sequence-table
locking, or hidden sequence-expression priority over pressure throttling. Those
remain planned sequence edge cases.
