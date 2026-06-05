# Ownerless CREATE OR REPLACE TABLE

## Problem

Ownerless table DDL coverage already proved peer refresh and reopen behavior for
`CREATE TABLE IF NOT EXISTS`, duplicate create failure, `DROP TABLE IF EXISTS`,
and repeated real-table drop. MariaDB also supports `CREATE OR REPLACE TABLE`,
which replaces an existing table under one SQL statement. That path combines
drop and create file lifecycle, native InnoDB dictionary refresh, and
ownerless dictionary-generation publication.

## Source Findings

- MariaDB 11.8 grammar accepts `create_or_replace ... TABLE` in
  `mariadb/sql/sql_yacc.yy`.
- MariaDB `sql_table.cc` has dedicated `CREATE OR REPLACE TABLE` handling and
  failure cleanup paths, so MyLite should not treat it as equivalent to a
  no-op idempotent create.
- The existing ownerless `table-idempotent-ddl` selector already has a live peer
  waiting on each DDL boundary and final ownerless/native reopen checks, making
  it a narrow place to add replacement-table evidence.

## Design

Extend the ownerless table-idempotent DDL sequence:

- create and populate the original InnoDB table from one ownerless process,
- keep another ownerless process open with the table cached,
- execute `CREATE OR REPLACE TABLE` from the DDL process with a different
  definition,
- verify the open peer observes the replacement definition, old rows are gone,
  new rows remain writable, and native `.frm`/`.ibd` files exist,
- continue through missing-table `DROP TABLE IF EXISTS`, repeated real drop,
  final ownerless/native reopen, forced `.shm` rebuild, and native reopen.

## Compatibility Impact

This adds coverage for a supported MariaDB table DDL spelling. It does not
change SQL policy or widen unsupported storage options.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test`.
- Run the focused `table-idempotent-ddl` selector.
- Run the matching embedded ownerless SQL CTest shard or selector.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- An already-open ownerless peer sees the replacement table definition.
- Rows from the replaced table are not visible after replacement.
- New rows written after replacement are visible through ownerless and ordinary
  native reopen before and after forced `.shm` rebuild.
- Final repeated `DROP TABLE IF EXISTS` still removes native `.frm`/`.ibd`
  files and leaves the table absent.
