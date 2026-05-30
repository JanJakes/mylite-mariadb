# Ownerless Column Default DDL Refresh

## Problem

Ownerless DDL coverage proves many table-shape and metadata changes refresh an
already-open peer, but column-default changes are a distinct MariaDB ALTER TABLE
path. Applications often change defaults without changing column type or
position. Ownerless mode needs evidence that `ALTER COLUMN ... SET DEFAULT` and
`ALTER COLUMN ... DROP DEFAULT` cross the shared dictionary-generation boundary
and affect subsequent peer DML immediately.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses
  `ALTER TABLE ... ALTER COLUMN col SET DEFAULT expr` and
  `ALTER TABLE ... ALTER COLUMN col DROP DEFAULT` through the alter-table
  grammar and records the change in the alter list.
- `mariadb/sql/sql_table.cc` maps parser-side default changes to
  `ALTER_COLUMN_DEFAULT` handler flags for ALTER TABLE execution.
- MyLite ownerless DDL marks the directory-backed dictionary generation active
  before MariaDB executes DDL and publishes a stable generation after success;
  peers refresh table metadata before the next statement that uses changed
  dictionary state.

## Scope And Non-Goals

- Add focused ownerless SQL coverage for `ALTER COLUMN ... SET DEFAULT` and
  `ALTER COLUMN ... DROP DEFAULT`.
- Verify an already-open peer inserts rows that use the original defaults, then
  the changed defaults, then fails when omitting a NOT NULL column after its
  default is dropped.
- Verify final default metadata and rows survive ownerless reopen, ordinary
  exclusive reopen, forced `.shm` rebuild, and exclusive reopen after rebuild.
- Do not change ownerless DDL implementation or claim every expression default
  variant.

## Design

Add a `column-default-ddl` selector to
`mylite_ownerless_cross_process_sql_test`:

1. A child ownerless process creates an InnoDB table with numeric and string
   column defaults and inserts one default-backed row.
2. The parent, already open in ownerless read/write mode, verifies the initial
   numeric default and inserts another row using the defaults.
3. The child sets the numeric and string defaults to new values.
4. The parent verifies the new numeric default, inserts a row using the changed
   defaults, and checks row aggregates.
5. The child drops the numeric default.
6. The parent verifies the numeric default is absent, proves omitting the
   NOT NULL numeric column now fails, inserts an explicit numeric value while
   still relying on the string default, and checks final rows.
7. Reopen helper assertions verify final metadata and rows through ownerless and
   native exclusive opens before and after forced `.shm` rebuild.

## Compatibility Impact

No public API changes. The slice strengthens partial ownerless ALTER TABLE
metadata compatibility for column-default changes while keeping broader DDL and
expression-default coverage partial.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises existing native InnoDB table
metadata and ownerless concurrency files inside the MyLite-owned database
directory.

## Native Storage Impact

Native storage remains MariaDB-managed. MyLite coordinates the ownerless DDL
boundary and verifies durable reopen behavior for the resulting native table.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `column-default-ddl` in `embedded-dev`.
- Build and run focused `column-default-ddl` in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer uses the original defaults before DDL.
- The peer uses the changed defaults after `SET DEFAULT`.
- Omitting a NOT NULL column fails after that column's default is dropped.
- Final metadata and rows survive ownerless/native reopen before and after
  forced `.shm` rebuild.
- Existing ownerless SQL and stress coverage remains green.

## Risks And Follow-Up

- This does not cover every expression default, generated default interaction,
  or randomized DDL oracle. Those remain separate compatibility gaps.
