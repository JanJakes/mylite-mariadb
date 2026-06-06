# Ownerless Column IF EXISTS Expression Crash

## Problem

Ownerless missing-column `IF EXISTS` crash coverage now proves simple
`MODIFY`, `RENAME`, `CHANGE`, and `ALTER COLUMN ... DEFAULT` no-op branches at
the dictionary-finish crash boundary. A remaining nearby gap is a missing-column
no-op on a table whose metadata includes generated-column and CHECK expressions
that reference the real column.

Recovery must preserve the existing table definition, dependent expressions,
CHECK enforcement, and rows if a writer dies after MariaDB accepts a missing
`RENAME COLUMN IF EXISTS` no-op but before MyLite publishes dictionary finish.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `RENAME COLUMN opt_if_exists_table_element ident TO ident` and adds an
  `Alter_column` entry through `LEX::add_alter_list()`.
- `mariadb/sql/sql_table.cc` `handle_if_exists_options()` removes
  missing-column `ALTER/RENAME COLUMN IF EXISTS` entries from
  `Alter_info::alter_list` after pushing `ER_BAD_FIELD_ERROR` as a note, then
  clears `ALTER_CHANGE_COLUMN_DEFAULT` when no alter-list entries remain.
- `mariadb/libmariadb/include/mysqld_error.h` defines
  `ER_BAD_FIELD_ERROR` as 1054.
- MyLite `packages/libmylite/src/database.cc`
  `ownerless_finish_dictionary_ddl()` pauses at `dictionary-before-finish`
  after successful SQL execution and before publishing the ownerless dictionary
  generation.
- Existing MyLite hook coverage for a real dependent-expression rename verifies
  generated-column and CHECK expression metadata after a successful column
  rename. This slice covers the missing-column no-op side of the same metadata
  class.

## Scope And Non-Goals

In scope:

- Add one unsafe-hook selector for missing
  `ALTER TABLE ... RENAME COLUMN IF EXISTS` on a table with stored and virtual
  generated columns plus named CHECK constraints that reference the real column.
- Verify recovery preserves the real column, keeps the missing and attempted
  renamed columns absent, keeps generated-column values and CHECK enforcement
  unchanged, keeps plain missing rename returning errno 1054, and accepts later
  writes.
- Verify ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Real dependent-expression rename crash recovery, already covered separately.
- Missing `CHANGE COLUMN IF EXISTS` and `ALTER COLUMN IF EXISTS SET/DROP
  DEFAULT` generated/CHECK expression variants.
- Foreign-key, period, partition, and online-DDL expression variants.
- SQL-level table-lock fault injection and external randomized DDL/RQG oracle
  execution.

## Design

Extend `mylite_ownerless_cross_process_sql_test` with a hook-only selector:

`dictionary-column-idempotent-rename-expression-crash`

1. Create an InnoDB table with:
   - real columns `base_value` and `adjust_value`;
   - `stored_sum` generated as `base_value + adjust_value`;
   - `virtual_product` generated as `base_value * adjust_value`;
   - CHECK constraints requiring `base_value > 0` and
     `base_value >= adjust_value`.
2. Kill a writer running
   `ALTER TABLE ... RENAME COLUMN IF EXISTS missing_base TO renamed_missing` at
   `dictionary-before-finish`.
3. Reopen with no live peer and verify:
   - `base_value` remains present;
   - `missing_base` and `renamed_missing` remain absent;
   - stored and virtual generated values still match the original expression;
   - CHECK constraints still reject invalid rows;
   - plain missing `RENAME COLUMN` fails with errno 1054;
   - later valid writes succeed.
4. Recheck the same state through ownerless and native opens before and after
   forced `.shm` recreation.

## Compatibility Impact

No SQL behavior changes are intended. This slice extends ownerless crash
evidence for MariaDB missing-column `IF EXISTS` no-op semantics on expression
metadata.

## Directory And Lifecycle Impact

No directory layout change. The selector exercises ownerless dictionary
recovery, volatile shared-memory rebuild, and native exclusive reopen against
InnoDB table metadata stored in the MyLite database directory.

## Native Storage Impact

No native storage format change. MariaDB removes the missing rename no-op entry
before table-definition mutation; MyLite verifies the unchanged generated-column
and CHECK metadata remains readable and enforceable after recovery.

## Public API, Build, Size, License, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `embedded-dev`.
- Build the same target with `ownerless-test-hooks`.
- Run focused hook selector:
  - `dictionary-column-idempotent-rename-expression-crash`
- Run adjacent hook selectors:
  - `dictionary-column-idempotent-rename-crash`
  - `dictionary-column-rename-expression-crash`
  - `dictionary-column-idempotent-change-crash`
- Run the ownerless hook CTest shard containing the new selector.
- Run `format-check`, `git diff --check`, and staged diff checks.

## Acceptance Criteria

- The focused selector reaches `dictionary-before-finish` and does not hang.
- Live-peer cleanup remains busy until no-live recovery.
- Recovery preserves real-column metadata, generated-column values, CHECK
  enforcement, missing-column absence, plain missing rename errno 1054, and
  post-recovery writes.
- Ownerless/native reopen before and after forced `.shm` rebuild observes the
  same final table state.

## Risks And Follow-Up

- This is focused missing-rename expression metadata coverage, not an exhaustive
  generated/CHECK matrix for all missing-column `IF EXISTS` spellings.
- Missing `CHANGE COLUMN IF EXISTS` and `ALTER COLUMN IF EXISTS SET/DROP
  DEFAULT` expression variants and external randomized DDL oracle execution
  remain planned.
