# Ownerless Schema Lifecycle Refresh

## Problem

Ownerless DDL coverage heavily exercises table-level dictionary changes inside
the existing `app` schema, but schema-level lifecycle is still only covered in
ordinary exclusive embedded tests. Ownerless peers also need evidence that
MariaDB `CREATE DATABASE` and `DROP DATABASE` boundaries refresh already-open
processes and leave durable directory state usable after no-live reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_db.cc:mysql_create_db_internal()` locks the schema name,
  creates the schema directory under the configured data home, and writes
  `db.opt`.
- `mariadb/sql/sql_db.cc:mysql_rm_db_internal()` locks the schema name, locks
  known tables and routines, drops tables through native DDL paths, deletes
  `db.opt`, and removes the schema directory.
- `mariadb/sql/mdl.cc` documents schema metadata locks as the protection used
  for database-level DDL such as `DROP DATABASE`.
- MyLite ownerless DDL classification in `packages/libmylite/src/database.cc`
  treats `CREATE` and `DROP` statements as dictionary DDL, so schema lifecycle
  changes publish through the same ownerless odd/even dictionary-generation
  protocol used by table DDL.
- The existing exclusive `metadata-ddl-lifecycle` slice proves MariaDB's schema
  directory and `db.opt` paths stay under `<db>.mylite/datadir/`; this slice
  adds cross-process ownerless refresh/reopen evidence for the schema boundary.

## Scope And Non-Goals

- Add a focused ownerless SQL selector for schema create/drop lifecycle.
- Verify an already-open ownerless peer observes a schema and InnoDB table
  created by another ownerless process and can write to that table.
- Verify the same peer observes `DROP DATABASE`, and final absence survives
  ownerless/native reopen before and after forced `.shm` rebuild.
- Do not add SQL-level table-lock fault injection.
- Do not claim full schema lifecycle coverage for routines, events, views,
  triggers, stored functions, grants, or crash recovery during schema DDL.

## Design

- Add `schema-lifecycle` to `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates `ownerless_schema`, creates an InnoDB
  table in it, inserts one row, then waits.
- The parent keeps an ownerless handle open, observes the new schema/table
  through `information_schema`, reads the row, and writes a second row.
- The child drops the schema. The parent observes schema/table absence from the
  same already-open handle and verifies the dropped qualified table is no
  longer selectable.
- After both ownerless handles close, helper assertions verify the absent schema
  and removed schema directory through:
  - `MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW`,
  - `MYLITE_OPEN_READWRITE`,
  - forced `concurrency/mylite-concurrency.shm` deletion plus ownerless reopen,
  - ordinary exclusive read/write reopen after the forced rebuild.

## Compatibility Impact

No SQL behavior changes. The compatibility matrix gains ownerless evidence for
representative schema create/drop lifecycle and cross-process dictionary refresh
while keeping broader schema DDL compatibility partial.

## Directory And Lifecycle Impact

The slice exercises MariaDB native schema metadata inside the MyLite-owned
database directory: `datadir/ownerless_schema/`, `db.opt`, `.frm`, and `.ibd`
files during the live phase, and verifies the schema directory is absent after
drop and no-live reopen.

## Native Storage Impact

No native storage format changes. The created table is InnoDB so the test also
exercises table-file deletion through MariaDB's native drop-database path while
ownerless dictionary generation and no-live recovery remain active.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `schema-lifecycle` selector.
- Run the focused `schema-lifecycle` selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage or a focused rerun if the
  known intermittent InnoDB log-header checksum abort appears.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks before commit.

## Acceptance Criteria

- Already-open ownerless peers see a schema and table created by another
  ownerless process.
- The peer can write through the newly visible schema/table.
- Already-open peers see the schema disappear after `DROP DATABASE`.
- Final schema absence and directory removal survive ownerless/native reopen
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- Schema routines, views, triggers, events, grants, and crash points during
  schema DDL remain outside this slice.
- Full DDL/file-lifecycle replay for missing tablespaces remains planned.
