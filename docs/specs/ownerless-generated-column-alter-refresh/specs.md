# Ownerless Generated Column ALTER Refresh

## Problem

Ownerless broader DDL coverage proves an already-open peer can use a table
created with generated columns. The column-shape ALTER slice deliberately left
generated-column ALTERs out of scope, so ownerless peers still lack bounded
evidence for generated columns added or dropped by another process after the
peer has opened the database and cached table metadata.

MyLite needs focused evidence that a peer refreshes both SQL table metadata and
InnoDB native generated-column metadata after `ALTER TABLE` adds stored and
virtual generated columns, and again after those generated columns are dropped.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `GENERATED ALWAYS AS (...)` field
  definitions and records generated-column metadata as virtual or stored
  through `VCOL_GENERATED_VIRTUAL` and `VCOL_GENERATED_STORED`.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` compares old and new field
  definitions, marks `ALTER_ADD_STORED_GENERATED_COLUMN`,
  `ALTER_ADD_VIRTUAL_COLUMN`, `ALTER_DROP_VIRTUAL_COLUMN`, generated-column
  expression changes, and other generated-column handler flags before handing
  the ALTER to the storage engine.
- `mariadb/storage/innobase/handler/handler0alter.cc` collects added and
  dropped virtual-column metadata in `prepare_inplace_add_virtual()` and
  `prepare_inplace_drop_virtual()`, and persists InnoDB dictionary changes via
  `SYS_COLUMNS` and `SYS_VIRTUAL` helper paths such as
  `innobase_add_virtual_try()` and `innobase_drop_virtual_try()`.
- `packages/libmylite/src/database.cc` treats ownerless `ALTER TABLE` as
  dictionary DDL: it publishes an odd dictionary generation while DDL is in
  progress and a stable even generation after execution, making peers wait for
  stable metadata before `FLUSH TABLES` and InnoDB dictionary-cache eviction.

## Scope And Non-Goals

- Add focused ownerless SQL coverage for `ALTER TABLE ... ADD COLUMN ...
  GENERATED ALWAYS AS` using one stored and one virtual generated column.
- Verify an already-open peer sees the generated columns, computes generated
  expressions from existing rows, and keeps generated values correct after peer
  writes to base columns.
- Verify an already-open peer sees generated columns dropped by another process
  and can continue writing base columns through the final table shape.
- Verify final base rows and absent generated-column metadata through
  ownerless/native reopen before and after forced `.shm` rebuild.
- Do not cover indexed generated columns, generated-column expression
  replacement, partitioning, generated columns in foreign keys beyond the
  stored generated-column FK shapes covered separately by
  `docs/specs/ownerless-generated-column-foreign-key/specs.md`, crash recovery
  during generated-column ALTER, or external MariaDB/RQG DDL oracle stress.

## Design

- Add a `generated-column-alter` selector to
  `mylite_ownerless_cross_process_sql_test`.
- A child ownerless process creates a base InnoDB table without generated
  columns and inserts a baseline row.
- The parent opens ownerless before the generated-column ALTERs, reads the base
  table, and stays open while the child adds generated columns.
- The child adds a stored `full_name` column and a virtual `name_length` column
  derived from base name columns.
- The parent observes the new columns through ordinary SELECTs and
  `INFORMATION_SCHEMA.COLUMNS`, updates base columns, inserts another row, and
  verifies generated expressions remain correct.
- The child drops both generated columns. The parent verifies metadata absence,
  continues writing base columns, and then final reopen helpers verify durable
  state through ownerless and ordinary exclusive opens before and after forced
  shared-memory rebuild.

## Compatibility Impact

This extends ownerless DDL evidence from create-time generated columns to
representative generated-column add/drop ALTER behavior. It does not claim the
full generated-column ALTER matrix or generated-column index semantics.

## Directory And Lifecycle Impact

No new files or layout changes. The slice exercises existing native InnoDB
metadata files under `datadir/` and ownerless dictionary-generation state under
`concurrency/`.

## Native Storage Impact

No native storage format changes. The test intentionally uses MariaDB/InnoDB's
native generated-column ALTER paths and verifies that MyLite's ownerless
dictionary refresh is sufficient for peers to use the updated native table
definition.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `generated-column-alter` selector.
- Build and run the focused `generated-column-alter` selector in
  `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage, ownerless stress, and the
  hook ownerless SQL label.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Already-open ownerless peers see generated columns added by another process.
- Generated expressions are correct for existing rows and after peer writes to
  base columns.
- Already-open ownerless peers see the generated columns removed after peer
  `DROP COLUMN` ALTERs.
- Final base rows and absent generated-column metadata survive ownerless/native
  reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- Indexed generated columns and generated-column expression replacement remain
  separate DDL coverage.
- Crash recovery during generated-column ALTER and external oracle stress remain
  broader DDL/recovery work.
