# Ownerless Generated Column Primary Key Policy

## Problem

Ownerless generated-column index coverage now proves deterministic stored and
virtual generated-column secondary indexes, including unique, prefix,
mixed-direction, and accepted online-option forms. The remaining primary-key
question is different: MariaDB rejects primary keys defined on generated
columns. MyLite ownerless mode should preserve that MariaDB-compatible policy,
surface the native errno, and leave no table or index side effects behind after
the failed DDL.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:2958-2963` rejects `Key::PRIMARY` when the target
  `Column_definition` has `vcol_info`, returning
  `ER_PRIMARY_KEY_BASED_ON_GENERATED_COLUMN`.
- `mariadb/sql/share/errmsg-utf8.txt` defines that error as "Primary key cannot
  be defined upon a generated column".
- `mariadb/libmariadb/include/mysqld_error.h` assigns
  `ER_PRIMARY_KEY_BASED_ON_GENERATED_COLUMN` errno `1903`.
- Ordinary generated-column secondary-index eligibility follows a different
  path through `Column_definition::check_vcol_for_key()` and InnoDB secondary
  index definition code; those supported forms remain covered by
  `ownerless-generated-column-index-ddl-refresh`.

## Scope And Non-Goals

In scope:

- Add a focused ownerless SQL selector named
  `generated-column-primary-key-policy`.
- Verify create-time stored and virtual generated-column primary-key
  definitions fail with MariaDB errno `1903`.
- Verify ownerless replacement-primary-key ALTERs that would add stored or
  virtual generated columns and promote them to `PRIMARY KEY` fail with errno
  `1903`.
- Verify existing stored and virtual generated columns cannot be promoted to
  `PRIMARY KEY`.
- Verify failed DDL leaves no rejected tables, generated replacement columns,
  or primary-key metadata side effects, and leaves the surviving tables usable.
- Verify the final policy state through ownerless/native reopen and forced
  `.shm` rebuild.

Out of scope:

- Adding support for generated-column primary keys; MariaDB rejects this shape.
- Generated-column expression replacement, nondeterministic-expression policy,
  successful generated-column DDL crash recovery, exhaustive online-option
  matrices, and external MariaDB/RQG oracle stress.

## Design

The selector uses one ownerless read/write handle:

1. Attempt create-time `PRIMARY KEY` definitions on stored and virtual generated
   columns, expecting errno `1903`.
2. Verify the rejected create-time tables do not exist.
3. Create an ordinary primary-key table, insert rows, and attempt two ALTER
   statements that add a stored or virtual generated column while replacing the
   primary key with that generated column. Each statement must fail with errno
   `1903`, preserve the original `PRIMARY(id)`, and not add the candidate
   generated column.
4. Create separate tables with existing stored and virtual generated columns
   and no explicit primary key, attempt `ALTER TABLE ... ADD PRIMARY KEY` on
   each generated column, and verify errno `1903` plus absent `PRIMARY`
   statistics.
5. Insert additional rows after the failed DDL to prove the surviving tables
   remain writable and generated values still calculate correctly.
6. Reopen the directory through ownerless and ordinary native read/write opens,
   then force `.shm` rebuild and repeat those checks.

## Compatibility Impact

This narrows generated-column primary-key status from an unproven gap to an
explicit MariaDB-compatible rejection in ownerless mode. MyLite does not support
generated-column primary keys because MariaDB 11.8 rejects them; the ownerless
claim is stable errno and side-effect-free failure.

## Directory And Lifecycle Impact

No new durable files or layout changes. The test verifies failed DDL does not
leave rejected tables, generated replacement columns, or primary-key metadata
behind in the MyLite database directory across ownerless/native reopen and
forced volatile shared-memory rebuild.

## Native Storage Impact

Native storage format is unchanged. InnoDB receives only the successful ordinary
tables and generated-column definitions that survive the rejected primary-key
DDL.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `generated-column-primary-key-policy` selector.
- Run adjacent generated-column selectors:
  `generated-column-index-ddl`, `generated-column-alter`,
  `generated-column-foreign-key`, and `generated-column-foreign-key-policy`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the full embedded ownerless SQL label, ownerless stress, `format-check`,
  `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Create-time stored and virtual generated-column primary-key definitions fail
  with MariaDB errno `1903` and leave no table behind.
- ALTER-time replacement-primary-key attempts that add stored or virtual
  generated columns fail with errno `1903`, preserve the original primary key,
  and do not add the generated candidate columns.
- Existing stored and virtual generated columns cannot be promoted to primary
  keys and leave no `PRIMARY` statistics after failure.
- Surviving tables remain writable and generated expressions continue to
  evaluate correctly after the failed DDL.
- Final state survives ownerless/native reopen and forced `.shm` rebuild.

## Risks And Follow-Up

- Failed generated-column primary-key DDL crash recovery is covered by
  `ownerless-generated-column-failed-ddl-crash`; successful generated-column
  DDL crash recovery and external oracle stress remain separate validation
  work.
