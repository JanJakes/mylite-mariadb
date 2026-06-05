# Ownerless Generated Column Nondeterministic Policy

## Problem

Ownerless generated-column coverage verifies deterministic generated-column
DDL, generated-column secondary indexes, same-kind expression replacement, and
MariaDB's generated-column primary-key rejection. The compatibility matrix still
tracks nondeterministic-expression policy as planned, so ownerless mode lacks
focused evidence that MariaDB's native generated-column expression restrictions
remain intact and leave no partially applied DDL state behind.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/field.cc:10817-10829` checks generated-column expressions with
  `check_expression()`. Stored generated columns add
  `VCOL_NOT_STRICTLY_DETERMINISTIC` to the rejection filter, while virtual
  generated columns do not.
- `mariadb/sql/table.cc:4015-4021` revalidates unpacked generated-column
  expressions and raises `ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED` when a
  non-virtual generated column contains a non-strictly-deterministic
  expression.
- `mariadb/sql/field.cc:11346-11351`
  `Column_definition::check_vcol_for_key()` rejects key use for generated
  columns whose expression flags include `VCOL_NOT_STRICTLY_DETERMINISTIC`.
- `mariadb/libmariadb/include/mysqld_error.h:892` defines
  `ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED` as errno 1901.
- `mariadb/mysql-test/suite/gcol/r/gcol_blocked_sql_funcs_innodb.result`
  records generated-column expression rejection with the MariaDB error text
  "Function or expression ... cannot be used in the GENERATED ALWAYS AS clause".

## Scope And Non-Goals

In scope:

- Add focused ownerless SQL coverage for stored generated-column
  nondeterministic-expression rejection at create time.
- Add ownerless ALTER coverage proving failed `ADD COLUMN` and same-kind
  `MODIFY COLUMN` attempts with a stored nondeterministic expression return
  errno 1901 and leave the prior table definition/data intact.
- Add ownerless coverage proving a virtual generated column with a
  nondeterministic expression may exist, but ordinary `CREATE INDEX` and
  `ALTER TABLE ... ADD INDEX` over that column return errno 1901 and leave no
  generated-column index behind.
- Verify side-effect-free state through ownerless and native reopen before and
  after forced `.shm` rebuild.

Out of scope:

- Indexed generated-column expression replacement, stored-to-virtual or
  virtual-to-stored conversion, exhaustive blocked SQL-function matrices,
  successful generated-column DDL crash recovery, and external MariaDB/RQG
  oracle stress.

## Design

Add a `generated-column-nondeterministic-policy` selector to
`mylite_ownerless_cross_process_sql_test`.

The selector opens an ownerless writer and verifies:

1. `CREATE TABLE` with a stored generated `RAND()` expression fails with errno
   1901 and leaves no table.
2. A deterministic stored generated-column table remains intact after failed
   `ALTER TABLE ... ADD COLUMN ... RAND() STORED` and failed same-kind
   `MODIFY COLUMN ... RAND() STORED`.
3. A table with a virtual generated `RAND()` expression remains writable, but
   both standalone `CREATE INDEX` and `ALTER TABLE ... ADD INDEX` over that
   generated column fail with errno 1901 and leave no index metadata.
4. Ownerless/native reopen checks, plus a forced shared-memory rebuild, verify
   table absence, column absence, index absence, and row aggregates.

## Compatibility Impact

This closes the ownerless nondeterministic generated-column policy gap for
representative MariaDB-native errno 1901 paths. It does not claim exhaustive
coverage of every function that MariaDB marks impossible, nondeterministic,
time-dependent, or session-dependent in generated-column expressions.

## Directory And Lifecycle Impact

No new durable files or layout changes. The slice verifies that failed
generated-column DDL does not leave native metadata artifacts in `datadir/` and
that ordinary ownerless/native reopen and forced `.shm` rebuild still observe
the same final state.

## Native Storage Impact

No native storage format changes. The test exercises MariaDB's existing
generated-column validation and InnoDB metadata side-effect behavior.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `generated-column-nondeterministic-policy` selector.
- Run adjacent generated-column selectors:
  `generated-column-alter`, `generated-column-index-ddl`, and
  `generated-column-primary-key-policy`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the embedded ownerless cross-process SQL label, ownerless stress,
  `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Ownerless stored generated-column create and ALTER paths reject
  nondeterministic stored expressions with MariaDB errno 1901.
- Failed stored generated-column DDL leaves no new table, column, or expression
  replacement behind.
- Ownerless virtual generated columns with nondeterministic expressions remain
  usable as non-indexed columns, while index creation over them rejects with
  errno 1901.
- Failed generated-column index DDL leaves no index metadata behind.
- Final state survives ownerless/native reopen before and after forced `.shm`
  rebuild.

## Risks And Follow-Up

- Exhaustive blocked-function matrices, indexed generated-column expression
  replacement, and generated-column conversion paths remain separate work.
- Failed generated-column DDL crash recovery is covered by
  `ownerless-generated-column-failed-ddl-crash`; successful generated-column
  DDL crash recovery and external MariaDB/RQG stress remain separate work.
