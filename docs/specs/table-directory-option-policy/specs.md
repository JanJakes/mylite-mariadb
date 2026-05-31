# Table Directory Option Policy

## Problem Statement

MyLite's storage contract is one MyLite-owned database directory. MariaDB table
DDL supports `DATA DIRECTORY` and `INDEX DIRECTORY` table options that can route
native engine files to caller-named filesystem locations or preserve external
path metadata. That is incompatible with MyLite's final single-directory
storage model and with ownerless recovery assumptions that native table files
live under the MyLite database directory.

This slice rejects those SQL options before MariaDB dispatch.

## Source Findings

MariaDB base ref: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:5882` parses table-level
  `DATA DIRECTORY` into `Lex->create_info.data_file_name` and marks
  `HA_CREATE_USED_DATADIR`.
- `mariadb/sql/sql_yacc.yy:5887` parses table-level
  `INDEX DIRECTORY` into `Lex->create_info.index_file_name` and marks
  `HA_CREATE_USED_INDEXDIR`.
- `mariadb/sql/sql_yacc.yy:5565` and `sql_yacc.yy:5567` also parse partition
  `DATA DIRECTORY` and `INDEX DIRECTORY` into partition file-name fields.
- `mariadb/sql/sql_yacc.yy:8150` allows `create_table_options_space_separated`
  inside `ALTER TABLE`, so the options are not limited to `CREATE TABLE`.
- `mariadb/sql/sql_table.cc:4685` handles unsupported `DATA DIRECTORY` and
  `INDEX DIRECTORY` cases for some create paths, and `sql_table.cc:4699`
  validates directory options before create proceeds.
- `mariadb/sql/sql_table.cc:11487` and `sql_table.cc:11495` preserve requested
  index/data directory paths through copy-style alter temporary names.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11193` validates InnoDB
  `DATA DIRECTORY`, requiring file-per-table and rejecting temporary tables.
- `mariadb/storage/innobase/handler/ha_innodb.cc:11616` handles compatibility
  between `DATA DIRECTORY`, `TABLESPACE`, and temporary-table creation.
- `mariadb/storage/myisam/ha_myisam.cc:856` reads table data/index symlink
  targets into `data_file_name` and `index_file_name`.

## Scope And Non-Goals

- Reject `CREATE TABLE ... DATA DIRECTORY`.
- Reject `CREATE TABLE ... INDEX DIRECTORY`.
- Reject `ALTER TABLE ... DATA DIRECTORY`.
- Reject `ALTER TABLE ... INDEX DIRECTORY`.
- Reject partition-level `DATA DIRECTORY` and `INDEX DIRECTORY` when present in
  table DDL.
- Keep ordinary application columns, aliases, string literals, and table names
  containing these words usable when they do not form the option sequence.
- Do not implement a MyLite-owned external tablespace registry or symlink
  relocation layer.
- Do not change MariaDB source grammar.

## Design

Add a first-party SQL policy predicate in `packages/libmylite/src/database.cc`.
For `CREATE` or `ALTER` statements whose early tokens contain `TABLE`, scan the
remaining identifier tokens for adjacent `DATA DIRECTORY` or
`INDEX DIRECTORY`. Return a MyLite policy error before MariaDB dispatch.

The policy is global, not ownerless-only, because durable table placement
outside the database directory violates the core storage contract even when only
one process has the directory open.

## Compatibility Impact

MariaDB accepts these options in server configurations where external table file
placement or symlinked MyISAM files are allowed. MyLite deliberately rejects
them. Applications should place the MyLite database directory itself wherever
they want durable data to live rather than directing individual table files to
separate paths.

## Directory And Native Storage Impact

No directory-format change. The policy prevents native InnoDB, MyISAM, Aria, or
partition files from being routed outside the database directory through SQL.

## Public API Impact

No new C API. `mylite_exec()` and `mylite_prepare()` return a MyLite policy
error for rejected SQL.

## Binary Size And Dependencies

No dependency or embedded profile change. The slice adds token checks and tests.

## Test Plan

- Extend the embedded server-surface/directory-boundary policy test with
  positive literals and negative `CREATE TABLE` / `ALTER TABLE` coverage for
  `DATA DIRECTORY` and `INDEX DIRECTORY`.
- Keep the existing no-unplanned-sidecar assertion after rejected statements.
- Run the focused embedded server-surface policy test.
- Run full embedded and hook ownerless SQL suites, ownerless stress,
  `format-check`, and diff whitespace checks.

## Acceptance Criteria

- Direct table DDL using `DATA DIRECTORY` or `INDEX DIRECTORY` fails with a
  MyLite policy error before MariaDB dispatch.
- Ordinary SQL literals containing these words still execute.
- No file or directory is created outside the MyLite database directory by the
  rejected SQL.
- Compatibility docs record the limitation explicitly.

## Risks And Follow-Up

- A future external tablespace feature would need a deliberate MyLite-owned
  directory lifecycle design, durable metadata, crash recovery, copy/backup
  rules, and ownerless peer coordination.
