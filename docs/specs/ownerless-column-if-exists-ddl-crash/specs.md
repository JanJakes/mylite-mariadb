# Ownerless Column IF EXISTS DDL Crash

## Problem

Ownerless hook coverage now kills duplicate
`ALTER TABLE ... ADD COLUMN IF NOT EXISTS` and missing
`ALTER TABLE ... DROP COLUMN IF EXISTS` no-op writers before dictionary finish.
MariaDB has additional missing-column `IF EXISTS` ALTER branches for column
definition changes and column rename/default alter-list operations. Those
branches still return successful SQL after removing the no-op element and still
cross MyLite's ownerless dictionary-generation boundary.

Recovery must preserve the existing table definition and data if a writer dies
after MariaDB accepts a missing-column no-op `MODIFY COLUMN IF EXISTS` or
`RENAME COLUMN IF EXISTS` statement but before MyLite publishes dictionary
finish.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `MODIFY opt_column opt_if_exists_table_element field_spec opt_place`, setting
  `ALTER_CHANGE_COLUMN` and recording the target field in
  `Create_field::change`.
- `mariadb/sql/sql_yacc.yy` parses
  `RENAME COLUMN opt_if_exists_table_element ident TO ident`, adding an
  `Alter_column` entry through `LEX::add_alter_list()`.
- `mariadb/sql/sql_table.cc` `handle_if_exists_options()` removes
  `MODIFY COLUMN IF EXISTS` entries when the referenced existing column is
  absent and pushes `ER_BAD_FIELD_ERROR` as a note.
- The same function removes missing `ALTER/RENAME COLUMN IF EXISTS`
  `Alter_column` entries and clears `ALTER_CHANGE_COLUMN_DEFAULT` when no
  alter-list entries remain.
- `mariadb/libmariadb/include/mysqld_error.h` defines
  `ER_BAD_FIELD_ERROR` as 1054.
- MyLite `packages/libmylite/src/database.cc`
  `ownerless_finish_dictionary_ddl()` pauses at `dictionary-before-finish`
  after successful SQL execution and before publishing the ownerless dictionary
  generation.

## Scope And Non-Goals

In scope:

- Add unsafe-hook selectors for missing-column `MODIFY COLUMN IF EXISTS` and
  `RENAME COLUMN IF EXISTS` no-op crash recovery.
- Kill a writer at `dictionary-before-finish` after
  `ALTER TABLE ... MODIFY COLUMN IF EXISTS missing_note ...` succeeds as a
  no-op.
- Kill a writer at `dictionary-before-finish` after
  `ALTER TABLE ... RENAME COLUMN IF EXISTS missing_note TO renamed_missing`
  succeeds as a no-op.
- Verify recovery preserves the real `note` column metadata/default, leaves the
  missing and attempted renamed columns absent, keeps plain non-idempotent
  retry spellings returning errno 1054, and accepts later writes.
- Verify ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Real column modify/rename crash recovery, already covered by separate
  selectors.
- Missing `CHANGE COLUMN IF EXISTS` and `ALTER COLUMN IF EXISTS SET/DROP
  DEFAULT` no-op crash recovery, covered by
  `docs/specs/ownerless-column-if-exists-change-default-crash/specs.md`.
- Generated-column/CHECK expression variants beyond the missing rename no-op
  covered by
  `docs/specs/ownerless-column-if-exists-expression-crash/specs.md`.
- Period, partition, index, and foreign-key variants.
- SQL-level table-lock fault injection; prior representative SQL shapes did not
  reach the ownerless table-wait callback.
- External randomized DDL/RQG oracle execution.

## Design

Extend `mylite_ownerless_cross_process_sql_test` with two hook-only selectors:

1. `dictionary-column-idempotent-modify-crash`
   - Create an InnoDB table with `note VARCHAR(16) NOT NULL DEFAULT 'stable'`.
   - Kill a writer running
     `ALTER TABLE ... MODIFY COLUMN IF EXISTS missing_note VARCHAR(64)`
     at `dictionary-before-finish`.
   - Reopen with no live peer and verify `note` still has its original type and
     default, `missing_note` is absent, later default-backed inserts use
     `'stable'`, and plain `MODIFY COLUMN missing_note` fails with 1054.
2. `dictionary-column-idempotent-rename-crash`
   - Create the same table shape.
   - Kill a writer running
     `ALTER TABLE ... RENAME COLUMN IF EXISTS missing_note TO renamed_missing`
     at `dictionary-before-finish`.
   - Reopen with no live peer and verify `note` remains present, both
     `missing_note` and `renamed_missing` remain absent, later writes using the
     real column succeed, and plain `RENAME COLUMN missing_note TO ...` fails
     with 1054.

Both selectors now use the held-live-peer crash helper and
`ownerless-column-if-exists-live-recovery` metadata proof to recover while
another ownerless process is live with the native file-operation marker clear,
then recheck the same state through ownerless and native opens before and after
`.shm` recreation.

## Compatibility Impact

No SQL feature behavior changes are intended. The slice strengthens partial
ownerless ALTER TABLE crash coverage for MariaDB missing-column `IF EXISTS`
no-op semantics.

## Directory And Lifecycle Impact

No directory layout change. The tests exercise ownerless dictionary live
recovery, volatile shared-memory rebuild, and native exclusive reopen against
InnoDB table metadata stored in the MyLite database directory.

## Native Storage Impact

No native storage format change. MariaDB removes the no-op ALTER entries before
InnoDB table-definition mutation; MyLite verifies recovery observes the
unchanged native table definition.

## Public API, Build, Size, License, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `embedded-dev`.
- Build the same target with `ownerless-test-hooks`.
- Run focused hook selectors:
  - `dictionary-column-idempotent-modify-crash`
  - `dictionary-column-idempotent-rename-crash`
- Run adjacent hook selectors for real column modify/rename and the prior
  idempotent column add/drop no-op crashes.
- Run the ownerless hook CTest shards containing the new selectors.
- Run `format-check`, `git diff --check`, and staged diff checks.

## Acceptance Criteria

- Both focused selectors reach `dictionary-before-finish` and do not hang.
- Missing modify/rename no-ops recover while a peer remains live.
- Missing `MODIFY COLUMN IF EXISTS` recovery preserves original real-column
  metadata/default, keeps the missing column absent, and keeps plain missing
  modify returning errno 1054.
- Missing `RENAME COLUMN IF EXISTS` recovery preserves the real column, keeps
  both the missing and attempted renamed columns absent, and keeps plain missing
  rename returning errno 1054.
- Ownerless/native reopen before and after forced `.shm` rebuild observes the
  same final table state.

## Risks And Follow-Up

- This is deterministic missing-column no-op ALTER TABLE crash coverage, not an
  exhaustive idempotent table-element matrix.
- Missing `CHANGE COLUMN IF EXISTS` and `ALTER COLUMN IF EXISTS SET/DROP
  DEFAULT` no-op crash recovery is covered by the follow-up
  `ownerless-column-if-exists-change-default-crash` slice.
- Missing rename generated-column/CHECK expression interaction coverage is
  tracked by `ownerless-column-if-exists-expression-crash`; other expression
  variants and external randomized DDL oracle execution remain planned.
