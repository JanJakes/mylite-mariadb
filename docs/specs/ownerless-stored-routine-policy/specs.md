# Ownerless Stored Routine Policy

## Problem

Ownerless DDL coverage now proves representative table, schema, view, and
trigger metadata refresh, but stored routine DDL follows a different MariaDB
system-table path. A proof attempt for ownerless `CREATE FUNCTION` and `DROP
FUNCTION` failed before it could establish refresh semantics: an already-open
peer hit MariaDB error 145 for `mysql.proc`, indicating that routine metadata
writes are not yet safe under the current ownerless coordination model.

Until MyLite designs cross-process coordination for MariaDB routine system
tables, ownerless read/write mode must fail closed for stored routine DDL
instead of letting routine metadata mutate accidentally.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documentation for stored functions
  (`https://mariadb.com/docs/server/server-usage/stored-routines/stored-functions/stored-function-overview`)
  describes stored functions as named routines callable from SQL expressions.
- MariaDB documentation for `CREATE FUNCTION`
  (`https://mariadb.com/docs/server/reference/sql-statements/data-definition/create/create-function`)
  distinguishes stored functions from UDF plugin registration.
- MariaDB documentation for `DROP FUNCTION`
  (`https://mariadb.com/kb/en/drop-function/`) says the statement removes a
  stored function or UDF and associated privileges.
- `mariadb/sql/sql_parse.cc` creates stored routines by calling
  `lex->sphead->m_handler->sp_create_routine()` and drops qualified stored
  routines through `drop_routine()`, which dispatches to
  `Sp_handler::sp_drop_routine()`.
- `mariadb/sql/sp.cc:Sp_handler::sp_create_routine()` takes routine metadata
  locks, verifies the schema, opens `mysql.proc` for update, stores the routine
  definition there, and can update `mysql.procs_priv`.
- `mariadb/sql/sp.cc:Sp_handler::sp_drop_routine()` opens `mysql.proc`, deletes
  the matching routine row, and invalidates the stored-routine cache through
  `sp_drop_routine_internal()`.
- `mariadb/sql/sp.cc:Sp_handler::sp_find_routine()` loads routines from
  `mysql.proc` into the per-thread routine cache when SQL statements call them.
- `mariadb/sql/sp.h` defines package specifications and package bodies as
  stored-program handlers with `SP_TYPE_PACKAGE` and `SP_TYPE_PACKAGE_BODY`,
  whose create/drop commands are `SQLCOM_CREATE_PACKAGE`,
  `SQLCOM_DROP_PACKAGE`, `SQLCOM_CREATE_PACKAGE_BODY`, and
  `SQLCOM_DROP_PACKAGE_BODY`.
- `mariadb/sql/sp.cc:Sp_handler_package_spec::sp_find_and_drop_routine()` drops
  a matching package body row when dropping a package specification, so package
  lifecycle uses the same `mysql.proc` metadata table as functions and
  procedures.
- MyLite ownerless DDL generation serialization is designed around the covered
  application dictionary/file refresh classes. It does not yet coordinate
  `mysql.proc`/`mysql.procs_priv` writes or routine-cache invalidation across
  processes.

## Scope And Non-Goals

- Reject ownerless read/write `CREATE`, `ALTER`, and `DROP` statements whose
  DDL target is a stored `FUNCTION`, `PROCEDURE`, `PACKAGE`, or `PACKAGE BODY`.
- Keep `CREATE FUNCTION ... SONAME` and aggregate UDF registration under the
  existing server-surface policy.
- Prove rejected routine DDL does not create visible `information_schema`
  routine metadata and leaves later ownerless/native reopen usable.
- Keep ordinary exclusive embedded stored-procedure behavior unchanged.
- Do not add ownerless support for stored functions, stored procedures, packages,
  routine privileges, routine cache invalidation, definer/security variants, or
  routine crash recovery.
- Do not add SQL-level table-lock fault injection; prior exploratory SQL shapes
  did not reach the ownerless table-wait callback.

## Design

- Add an ownerless-only SQL policy predicate in `packages/libmylite/src/database.cc`.
- Tokenize through the existing SQL policy tokenizer and inspect top-level
  `ALTER`, `CREATE`, and `DROP` DDL.
- Reject statements where the DDL target is `FUNCTION`, `PROCEDURE`, or
  `PACKAGE`; `PACKAGE BODY` is caught at the leading `PACKAGE` token.
- Stop scanning when another DDL object class such as `TABLE`, `TRIGGER`,
  `VIEW`, `SCHEMA`, `SEQUENCE`, or `SERVER` is identified, so ordinary covered
  ownerless DDL continues to reach MariaDB.
- Surface `MYLITE_ERROR` with a stable diagnostic that ownerless read/write
  mode does not support stored routine DDL.
- Add a focused `routine-policy` selector in
  `mylite_ownerless_cross_process_sql_test`.

## Compatibility Impact

Ownerless read/write mode explicitly does not support stored routine DDL. This
is a compatibility reduction only for the ownerless cross-process mode; ordinary
exclusive embedded stored-procedure create/show/call/drop coverage remains the
compatibility evidence for the current routine subset.

Stored functions, packages, broader routine metadata compatibility, and routine
edge cases remain partial/planned until routine system-table coordination is
designed; prepared `CALL` remains unsupported and is covered by the
routine-execution policy.

## Directory And Lifecycle Impact

The policy prevents ownerless routine DDL from mutating `datadir/mysql/proc.*`
and related routine privilege metadata. It adds no new durable files and keeps
all rejected-state verification inside the MyLite-owned database directory.

The focused test verifies the database can be reopened through:

- ownerless read/write mode,
- ordinary exclusive read/write mode,
- ownerless read/write mode after forced `concurrency/mylite-concurrency.shm`
  deletion,
- ordinary exclusive read/write mode after that forced rebuild.

## Native Storage Impact

No native storage format changes. The slice is a guard around unsafe routine
system-table metadata writes, not a storage-engine feature.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `routine-policy` selector.
- Build and run the focused `routine-policy` selector in
  `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage, rerunning focused selectors
  and the full label if the known intermittent InnoDB log-header checksum abort
  appears.
- Run the ownerless stress preset, `format-check`, `git diff --check`, and
  cached diff checks before commit.

## Acceptance Criteria

- Ownerless read/write `CREATE FUNCTION`, `CREATE OR REPLACE FUNCTION`,
  `CREATE PROCEDURE`, `CREATE PACKAGE`, `CREATE PACKAGE BODY`, `DROP FUNCTION`,
  `DROP PROCEDURE`, `DROP PACKAGE BODY`, `DROP PACKAGE`, `ALTER FUNCTION`, and
  `ALTER PROCEDURE` fail before MariaDB mutates routine metadata.
- Rejected statements return a MyLite policy error, not a MariaDB system-table
  error.
- `information_schema.routines` shows no rejected routine objects.
- Calls to the rejected function/procedure fail.
- The final no-routine state survives ownerless/native reopen before and after
  forced shared-memory rebuild.
- Compatibility docs mark ownerless stored routine DDL unsupported while
  retaining the broader routine gaps as planned.

## Risks And Follow-Up

- Future ownerless routine support needs a design for `mysql.proc`,
  `mysql.procs_priv`, stored-routine cache invalidation, routine privileges,
  routine MDL, and crash recovery.
- Broader DDL-created file lifecycle recovery and external MariaDB/RQG stress
  remain planned ownerless-concurrency gaps.
