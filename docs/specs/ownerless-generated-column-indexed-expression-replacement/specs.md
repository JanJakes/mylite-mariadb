# Ownerless Generated Column Indexed Expression Replacement

## Problem

Ownerless generated-column coverage already verifies same-kind expression
replacement for unindexed generated columns and secondary-index create/use/drop
over deterministic generated columns. The compatibility matrix still tracked
indexed generated-column expression replacement as planned, so ownerless mode
did not yet prove that replacing a generated expression while its secondary
index remains present refreshes peer metadata and rebuilds native index contents
coherently.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/field.cc:10800-10836` validates generated-column expressions
  through `check_expression()`, and `mariadb/sql/field.cc:10937-10949` calls
  it for generated columns during column definition checks.
- `mariadb/sql/sql_table.cc:7178-7221` compares old and new generated-column
  definitions during `ALTER TABLE`, sets `ALTER_STORED_GCOL_EXPR` or
  `ALTER_VIRTUAL_GCOL_EXPR` when the expression differs, and marks
  `ALTER_COLUMN_VCOL` when changed stored, partition-key, or indexed generated
  values must be rebuilt.
- `mariadb/sql/handler.h:799-801` defines the generated-expression handler
  flags used by storage engines for virtual and stored generated-column
  expression changes.
- `mariadb/mysql-test/suite/gcol/inc/gcol_column_def_options.inc:455-464`
  covers accepted replacement of indexed virtual and stored generated-column
  expressions, and
  `mariadb/mysql-test/suite/gcol/inc/gcol_column_def_options.inc:480-487`
  covers duplicate-key rejection when a unique generated-column index would
  become invalid after expression replacement.

## Scope And Non-Goals

In scope:

- Add ownerless SQL coverage for replacing an indexed stored generated-column
  expression from another ownerless process while a peer has the table open.
- Add ownerless SQL coverage for replacing an indexed virtual generated-column
  expression from another ownerless process while a peer has the table open.
- Verify generated-column secondary index metadata remains visible, forced
  index reads use recalculated generated values, and peer DML after replacement
  uses the replacement expressions.
- Verify final state through ownerless and native reopen before and after
  forced `.shm` rebuild.

Out of scope:

- Unique-index duplicate rejection during expression replacement, generated
  stored-to-virtual or virtual-to-stored conversion, exhaustive online-option
  combinations, crash injection during expression replacement, and external
  MariaDB/RQG oracle stress.

## Design

Add a `generated-column-indexed-expression` selector to
`mylite_ownerless_cross_process_sql_test`.

The selector uses two ownerless processes:

1. A child creates an InnoDB table with deterministic stored and virtual
   generated columns, then creates ordinary secondary indexes over both.
2. The already-open parent verifies peer-visible index metadata and forced-index
   reads over the original expressions, then mutates base rows.
3. The child replaces the stored generated expression while the stored index
   remains present. The parent verifies the stored index now answers predicates
   over the replacement expression while the virtual index still uses its
   original expression.
4. The parent performs another base-column update. The child then replaces the
   virtual generated expression while the virtual index remains present. The
   parent verifies both indexes answer predicates over the replacement
   expressions and inserts another row through the refreshed metadata.
5. Ownerless/native reopen checks, plus forced `.shm` rebuild, verify the final
   generated values, index metadata, forced-index reads, and a temporary
   insert/delete through the replacement expressions.

## Compatibility Impact

This closes the ownerless indexed generated-column expression replacement gap
for representative accepted MariaDB paths over ordinary secondary indexes. It
does not claim exhaustive coverage for unique-index failure handling, every
online DDL option, conversion between virtual and stored generated columns, or
crash recovery inside the ALTER.

## Directory And Lifecycle Impact

No new durable files or directory layout changes. The slice verifies that native
InnoDB table and index metadata created by the ALTER remains inside the MyLite
database directory and is visible after ordinary ownerless/native reopen and
forced shared-memory rebuild.

## Native Storage Impact

No native storage format changes. The test exercises MariaDB and InnoDB native
ALTER handling for generated expression changes while existing secondary
indexes over the generated columns remain defined.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `generated-column-indexed-expression`.
- Run adjacent generated-column selectors:
  `generated-column-alter`, `generated-column-index-ddl`,
  `generated-column-primary-key-policy`, and
  `generated-column-nondeterministic-policy`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the embedded ownerless cross-process SQL label, ownerless stress,
  `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Ownerless replacement of an indexed stored generated-column expression
  refreshes an already-open peer and preserves usable secondary-index metadata.
- Ownerless replacement of an indexed virtual generated-column expression
  refreshes an already-open peer and preserves usable secondary-index metadata.
- Forced-index reads observe recalculated generated values after expression
  replacement and after later peer DML changes base columns.
- Final state survives ownerless/native reopen before and after forced `.shm`
  rebuild, including a temporary post-reopen insert/delete through the
  replacement expressions.

## Risks And Follow-Up

- Unique-index duplicate rejection during expression replacement, generated
  kind conversion, crash injection during ALTER, exhaustive online-option
  matrices, and external MariaDB/RQG stress remain separate work.
