# Ownerless Table Comment DDL Refresh

## Problem

Ownerless DDL coverage includes table-shape changes, indexes, constraints, row
format, and charset conversion. Table comments are a separate ALTER TABLE
create-option path: applications and migration tools can update
`TABLE_COMMENT` without changing row layout. Ownerless mode needs evidence that
this metadata-only table option refreshes already-open peers and remains
durable across reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses table-option `COMMENT` and records
  `HA_CREATE_USED_COMMENT` in `HA_CREATE_INFO`.
- `mariadb/sql/sql_table.cc` carries explicit table comments through ALTER
  TABLE preparation and preserves the old comment when no new comment is
  supplied.
- `mariadb/sql/handler.cc` includes `ALTER_CHANGE_CREATE_OPTION` in the
  generic in-place ALTER operation set.
- `mariadb/sql/sql_show.cc` exposes the table comment through
  `information_schema.TABLES.TABLE_COMMENT`.
- MyLite ownerless DDL marks the directory-backed dictionary generation active
  before MariaDB executes DDL and publishes a stable generation after success;
  peers refresh table metadata before the next statement that uses changed
  dictionary state.

## Scope And Non-Goals

- Add focused ownerless SQL coverage for
  `ALTER TABLE ... COMMENT='ownerless updated comment'`.
- Verify an already-open ownerless peer observes the original and updated
  comments through `information_schema.TABLES.TABLE_COMMENT`.
- Verify existing rows remain readable and the already-open peer can insert
  after the comment metadata boundary.
- Verify final metadata and rows survive ownerless reopen, ordinary exclusive
  reopen, forced `.shm` rebuild, and exclusive reopen after rebuild.
- Do not claim every table option, comment character-set edge case, or
  randomized DDL oracle coverage.

## Design

Add a `table-comment-ddl` selector to
`mylite_ownerless_cross_process_sql_test`:

1. A child ownerless process creates an InnoDB table with an initial table
   comment, inserts rows, and signals the parent.
2. The already-open parent verifies the initial `TABLE_COMMENT` metadata and
   reads the inserted rows.
3. The child runs `ALTER TABLE ... COMMENT='ownerless updated comment'`.
4. The parent verifies the updated `TABLE_COMMENT`, inserts a row, and checks
   final row aggregates.
5. Reopen helper assertions verify final comment metadata and rows through
   ownerless/native reopen before and after forced `.shm` rebuild.

## Compatibility Impact

No public API behavior changes. The slice strengthens partial ownerless
DDL/dictionary compatibility for a metadata-only table option while leaving
broader DDL/file-lifecycle recovery and external oracle stress planned.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises existing native InnoDB metadata
and ownerless concurrency files under the MyLite-owned database directory.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. MyLite coordinates the ownerless
DDL boundary and verifies durable reopen behavior for the changed table
metadata.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `table-comment-ddl` in `embedded-dev`.
- Build and run focused `table-comment-ddl` in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- The already-open peer observes the initial table comment before the ALTER.
- The already-open peer observes the updated table comment after the ALTER.
- Existing rows remain readable and post-ALTER peer DML persists.
- Final metadata and rows survive ownerless/native reopen before and after
  forced `.shm` rebuild.
- Existing ownerless SQL and stress coverage remains green.

## Risks And Follow-Up

- This does not cover every table option or comment encoding edge case.
- Broader randomized DDL oracles remain planned.
