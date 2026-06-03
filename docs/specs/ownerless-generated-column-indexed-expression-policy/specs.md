# Ownerless Generated Column Indexed Expression Policy

## Problem

Ownerless coverage now verifies accepted generated-column expression
replacement while ordinary secondary indexes over stored and virtual generated
columns remain present. MariaDB also rejects expression replacement when the
replacement values would violate an existing unique generated-column index. The
ownerless harness needs focused evidence that those duplicate-key failures
return MariaDB errno 1062 and leave the original generated expression and unique
index metadata intact.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:7178-7221` marks generated-column expression
  replacement with `ALTER_STORED_GCOL_EXPR` or `ALTER_VIRTUAL_GCOL_EXPR` when
  the old and new expressions differ.
- `mariadb/sql/handler.h:799-801` defines the generated-expression handler
  flags that storage engines receive for these ALTER paths.
- `mariadb/mysql-test/suite/gcol/inc/gcol_column_def_options.inc:472-487`
  creates unique indexes over virtual and stored generated columns, then
  expects `ER_DUP_ENTRY` when replacing each expression with one that produces
  duplicate generated values.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already uses
  `MYLITE_TEST_DUPLICATE_KEY_ERRNO` for MariaDB errno 1062 duplicate-key
  assertions.

## Scope And Non-Goals

In scope:

- Add ownerless SQL coverage for duplicate-key rejection while replacing an
  indexed stored generated-column expression under an existing unique index.
- Add ownerless SQL coverage for duplicate-key rejection while replacing an
  indexed virtual generated-column expression under an existing unique index.
- Verify the original generated expressions, unique index metadata, forced-index
  reads, and writable post-failure state through ownerless/native reopen before
  and after forced `.shm` rebuild.

Out of scope:

- Exhaustive duplicate-value distributions, conversion between stored and
  virtual generated columns, crash injection inside the failed ALTER, exhaustive
  online-option matrices, and external MariaDB/RQG oracle stress.

## Design

Add a `generated-column-indexed-expression-policy` selector to
`mylite_ownerless_cross_process_sql_test`.

The selector creates two ownerless InnoDB tables:

1. A stored generated-column table with unique values from `col2 - col3` and a
   unique index over `stored_key`.
2. A virtual generated-column table with unique values from `col2 + col3` and a
   unique index over `virtual_key`.

For each table, the selector attempts to replace the generated expression with
`col2 DIV col3`, which maps two existing rows to the same generated value. The
ALTER must fail with `MYLITE_TEST_DUPLICATE_KEY_ERRNO`/1062. The test then
verifies the old expression is still active with forced-index reads, and the
reopen helper repeats those checks through ownerless/native reopen before and
after forced shared-memory rebuild. The helper temporarily inserts and deletes a
new row to prove writes still use the original unique generated expression after
the failed ALTER.

## Compatibility Impact

This closes a representative ownerless policy gap for MariaDB-compatible
duplicate-key rejection during indexed generated-column expression replacement.
It does not claim exhaustive unique-index or online-DDL option coverage.

## Directory And Lifecycle Impact

No directory layout changes. The test verifies that failed ALTERs do not leave
replacement generated-column metadata or rebuilt unique-index artifacts that
change the final state observed through ordinary reopen and forced `.shm`
rebuild.

## Native Storage Impact

No native storage format changes. The slice exercises MariaDB/InnoDB native
duplicate-key validation during generated-expression replacement.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `generated-column-indexed-expression-policy`.
- Run adjacent generated-column selectors:
  `generated-column-indexed-expression`, `generated-column-index-ddl`,
  `generated-column-primary-key-policy`, and
  `generated-column-nondeterministic-policy`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the embedded ownerless cross-process SQL label, ownerless stress,
  `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Stored generated-column expression replacement that would duplicate values
  under an existing unique generated-column index fails with errno 1062.
- Virtual generated-column expression replacement that would duplicate values
  under an existing unique generated-column index fails with errno 1062.
- Failed ALTERs leave the original generated expressions and unique index
  metadata usable by forced-index reads.
- Final state survives ownerless/native reopen before and after forced `.shm`
  rebuild, and later writes still use the original unique generated expressions.

## Risks And Follow-Up

- Conversion paths, crash injection during failed ALTER, exhaustive online
  option matrices, and external MariaDB/RQG stress remain separate work.
