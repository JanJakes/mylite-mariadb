# Ownerless Tablespace Management Policy

## Problem

Ownerless read/write mode coordinates ordinary InnoDB DDL through dictionary
generation, page-version WAL, native checkpoint evidence, and conservative file
refresh/replay. `ALTER TABLE ... DISCARD TABLESPACE` and
`ALTER TABLE ... IMPORT TABLESPACE` are a different native file-lifecycle
surface: they detach or attach a table's InnoDB tablespace file and rely on
external file handling around the SQL statement.

MyLite does not yet have durable ownerless file-lifecycle metadata for explicit
tablespace detach/import operations. Ownerless mode must reject those statements
before MariaDB enters the handler path, while ordinary exclusive read/write mode
continues to inherit MariaDB behavior.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `DISCARD TABLESPACE` and
  `IMPORT TABLESPACE` as `ALTER TABLE` operations.
- `mariadb/sql/sql_alter.h` defines
  `Sql_cmd_discard_import_tablespace`, and `mariadb/sql/sql_alter.cc` routes
  execution to `mysql_discard_or_import_tablespace()`.
- `mariadb/sql/sql_table.cc:mysql_discard_or_import_tablespace()` marks
  `thd->tablespace_op` and calls the handler's
  `ha_discard_or_import_tablespace()`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:
  ha_innobase::discard_or_import_tablespace()` implements the InnoDB
  detach/import path, including checks for missing or existing tablespace
  files.
- MyLite product no-live replay can skip retained page-version records for
  tablespaces that no longer exist, but it does not persist a durable
  ownerless file-lifecycle log that could make explicit detach/import safe with
  live peers or across crash boundaries.

## Scope And Non-Goals

- Reject ownerless `ALTER TABLE ... DISCARD TABLESPACE`.
- Reject ownerless `ALTER TABLE ... IMPORT TABLESPACE`.
- Verify the rejected statements leave the InnoDB table data and `.ibd` file
  intact through ownerless reopen, native exclusive reopen, and forced `.shm`
  rebuild.
- Do not implement ownerless tablespace detach/import coordination, external
  file-copy workflow, or durable DDL file-lifecycle replay.
- Do not change ordinary exclusive read/write MariaDB behavior.

## Design

- Add an ownerless SQL policy predicate for `ALTER TABLE` statements.
- After the `TABLE` keyword, reject `DISCARD TABLESPACE` and
  `IMPORT TABLESPACE` token sequences.
- Return a MyLite policy error before MariaDB dispatch, matching the existing
  ownerless unsupported-policy pattern where MariaDB errno remains zero.
- Add a focused `tablespace-policy` ownerless SQL selector that creates a normal
  InnoDB table, inserts a row, rejects both tablespace-management statements,
  and verifies the table row plus native `.ibd` file through ownerless/native
  reopen before and after forced shared-memory rebuild.

## Compatibility Impact

Ownerless read/write mode makes explicit tablespace detach/import unsupported
instead of accidentally entering native file-lifecycle paths. This is a
compatibility limitation, but it is safer than allowing unproven external file
operations while ownerless peers and page-version WAL may still reference the
tablespace.

Ordinary exclusive read/write opens are unchanged.

## Directory And Lifecycle Impact

No new files or directory layout changes are introduced. The policy prevents
ownerless SQL from deleting or importing native InnoDB tablespace files before
MyLite has a durable file-lifecycle protocol for that class.

## Native Storage Impact

Supported ordinary InnoDB table storage is unchanged. Explicit InnoDB
tablespace detach/import remains future work for ownerless mode.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `tablespace-policy` in `embedded-dev`.
- Build and run focused `tablespace-policy` in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- Ownerless `ALTER TABLE ... DISCARD TABLESPACE` and
  `ALTER TABLE ... IMPORT TABLESPACE` fail with a MyLite policy error before
  MariaDB dispatch.
- Rejected statements leave the base table row and `.ibd` file intact through
  ownerless/native reopen before and after forced `.shm` rebuild.
- Existing ownerless SQL and stress coverage remains green.

## Risks And Follow-Up

- Full support needs durable ownerless file-lifecycle metadata, explicit
  external-file workflow rules, tablespace identity validation, crash recovery
  tests, and external oracle stress.
