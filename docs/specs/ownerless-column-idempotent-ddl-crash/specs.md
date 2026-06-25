# Ownerless Column Idempotent DDL Crash

## Problem

Ownerless column idempotent DDL coverage proves already-open peer refresh for
`ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and
`ALTER TABLE ... DROP COLUMN IF EXISTS`, but hook crash coverage only covers
real column add/drop/modify/rename transitions. It does not yet kill a writer
after MariaDB has accepted a duplicate idempotent column add or a missing
idempotent column drop but before MyLite publishes ownerless dictionary finish.

These no-op ALTER statements still cross the ownerless dictionary generation
boundary. Recovery must preserve the pre-existing table definition rather than
partially applying the attempted duplicate column definition or forgetting the
real column after a missing-column drop.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `ADD opt_column opt_if_not_exists_table_element` and
  `DROP opt_column opt_if_exists_table_element field_ident`, setting
  `ALTER_PARSER_ADD_COLUMN` or `ALTER_PARSER_DROP_COLUMN`.
- `mariadb/sql/sql_table.cc` `handle_if_exists_options()` removes duplicate
  `ADD COLUMN IF NOT EXISTS` entries when the column already exists and pushes
  `ER_DUP_FIELDNAME` as a note.
- The same function removes missing `DROP COLUMN IF EXISTS` entries and pushes
  `ER_CANT_DROP_FIELD_OR_KEY` as a note.
- `mariadb/libmariadb/include/mysqld_error.h` defines `ER_DUP_FIELDNAME` as
  1060 and `ER_CANT_DROP_FIELD_OR_KEY` as 1091.
- MyLite `packages/libmylite/src/database.cc`
  `ownerless_finish_dictionary_ddl()` pauses at `dictionary-before-finish`
  after successful SQL execution and before publishing the ownerless dictionary
  generation.

## Scope And Non-Goals

In scope:

- Add unsafe-hook selectors for duplicate column add and missing column drop
  idempotent no-op crash recovery.
- Kill a writer at `dictionary-before-finish` for
  `ALTER TABLE ... ADD COLUMN IF NOT EXISTS note ...` when `note` already
  exists.
- Verify recovery preserves the original `note` column default, keeps plain
  duplicate `ADD COLUMN` returning errno 1060, and accepts later default-backed
  inserts.
- Kill a writer at `dictionary-before-finish` for
  `ALTER TABLE ... DROP COLUMN IF EXISTS missing_note` when the real `note`
  column exists.
- Verify recovery preserves the real column, keeps the missing column absent,
  and accepts later writes using the preserved column.
- Verify ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Real column add/drop crash recovery, already covered by separate selectors.
- Idempotent `CHANGE`, `MODIFY`, `ALTER COLUMN`, `RENAME COLUMN`, period,
  partition, index, foreign-key, and CHECK table-element variants.
- SQL-level table-lock fault injection; prior representative SQL shapes did
  not reach the ownerless table-wait callback.
- External randomized DDL/RQG oracle execution.

## Design

Extend `mylite_ownerless_cross_process_sql_test` with two hook-only selectors:

1. `dictionary-column-idempotent-add-crash`
   - Create an InnoDB table with `note INT NOT NULL DEFAULT 7`.
   - Kill a writer running duplicate
     `ALTER TABLE ... ADD COLUMN IF NOT EXISTS note INT NOT NULL DEFAULT 99`
     at `dictionary-before-finish`.
   - Reopen with no live peer and verify `note` still has default `7`, later
     default-backed inserts use `7`, and plain duplicate add fails with 1060.
2. `dictionary-column-idempotent-drop-crash`
   - Create an InnoDB table with a real `note` column and no `missing_note`.
   - Kill a writer running
     `ALTER TABLE ... DROP COLUMN IF EXISTS missing_note` at
     `dictionary-before-finish`.
   - Reopen with no live peer and verify `note` remains present and writable,
     while `missing_note` remains absent.

Both selectors now use the held-live-peer crash helper and
`ownerless-column-idempotent-live-recovery` metadata proof to recover while
another ownerless process is live with the native file-operation marker clear,
then recheck state through ownerless and native opens before and after `.shm`
recreation.

## Compatibility Impact

No SQL feature behavior changes are intended. The slice strengthens partial
ownerless ALTER TABLE crash coverage for MariaDB idempotent column no-op
semantics.

## Directory And Lifecycle Impact

No directory layout change. The tests exercise ownerless dictionary live
recovery, volatile shared-memory rebuild, and native exclusive reopen against
InnoDB table metadata stored in the MyLite database directory.

## Native Storage Impact

No native storage format change. MariaDB removes the no-op ALTER elements before
InnoDB table-definition mutation; MyLite verifies recovery observes the
unchanged native table definition.

## Public API, Build, Size, License, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `embedded-dev`.
- Build the same target with `ownerless-test-hooks`.
- Run focused hook selectors:
  - `dictionary-column-idempotent-add-crash`
  - `dictionary-column-idempotent-drop-crash`
- Run adjacent hook selectors for real column add/drop and idempotent index
  no-op crashes.
- Run the ownerless hook CTest shard containing the new selectors.
- Run `format-check`, `git diff --check`, and staged diff checks.

## Acceptance Criteria

- Both focused selectors reach `dictionary-before-finish` and do not hang.
- Duplicate add and missing drop recover while a peer remains live.
- Duplicate idempotent add recovery preserves original column metadata and
  default-backed writes.
- Missing idempotent drop recovery preserves the real column and leaves the
  missing column absent.
- Ownerless/native reopen before and after forced `.shm` rebuild observes the
  same final table state.

## Risks And Follow-Up

- This is deterministic no-op ALTER TABLE crash coverage, not randomized DDL
  oracle execution.
- Broader idempotent table-element crash matrices remain planned.
