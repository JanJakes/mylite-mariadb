# Ownerless Special Index Policy

## Problem

Ownerless concurrency currently has evidence for ordinary InnoDB secondary
index DDL, including standalone `CREATE INDEX` / `DROP INDEX` refresh. MariaDB
`FULLTEXT` and `SPATIAL` indexes are separate special index classes with
different native storage, search, and lock behavior. MyLite does not yet have
ownerless evidence for InnoDB full-text auxiliary state, spatial R-tree pages,
or spatial predicate locking across independent embedded processes.

Ownerless read/write mode should reject `FULLTEXT` and `SPATIAL` index DDL
before MariaDB mutates special index metadata or native storage.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB `CREATE INDEX` documentation
  (`https://mariadb.com/kb/v/create-index/`) describes `FULLTEXT` and
  `SPATIAL` as explicit index types in `CREATE [OR REPLACE]
  [UNIQUE|FULLTEXT|SPATIAL] INDEX`.
- MariaDB full-text index documentation
  (`https://mariadb.com/kb/en/full-text-index-overview/`) says `FULLTEXT`
  definitions can appear in `CREATE TABLE`, `ALTER TABLE`, or `CREATE INDEX`.
- MariaDB spatial index documentation
  (`https://mariadb.com/kb/en/spatial-index/`) says spatial indexes can be
  created at table creation time, added by `ALTER TABLE`, or added by
  `CREATE SPATIAL INDEX`, and that they create an R-tree index for engines that
  support it.
- `mariadb/sql/sql_yacc.yy` parses top-level `CREATE ... FULLTEXT INDEX` and
  `CREATE ... SPATIAL INDEX`, plus inline or `ALTER TABLE` index definitions
  through the `fulltext` and `spatial_or_vector` grammar paths.
- `mariadb/storage/innobase/handler/ha_innodb.cc` advertises InnoDB full-text
  capability, and InnoDB full-text code maintains auxiliary table/index state.
- `mariadb/storage/innobase/gis/gis0sea.cc` and related R-tree paths implement
  spatial index search/insert behavior. The ownerless lock-registry spec
  explicitly does not support spatial predicate locks.
- The existing ownerless standalone-index slice deliberately scoped itself to a
  plain secondary index and left full-text and spatial variants planned.

## Scope And Non-Goals

- Reject ownerless read/write `CREATE FULLTEXT INDEX` and `CREATE SPATIAL
  INDEX`, including `CREATE OR REPLACE FULLTEXT INDEX`.
- Reject ownerless read/write `ALTER TABLE ... ADD FULLTEXT INDEX` and `ALTER
  TABLE ... ADD SPATIAL INDEX`.
- Reject ownerless read/write inline `FULLTEXT` and `SPATIAL` index definitions
  in `CREATE TABLE`.
- Verify rejection is a MyLite policy error before MariaDB execution.
- Verify rejected inline table creation does not leave application tables, and
  rejected special indexes do not appear in `information_schema.statistics`.
- Do not add ownerless support for full-text search, spatial predicates, R-tree
  page-version reclamation beyond existing conservative handling, full-text
  auxiliary table recovery, or special-index `DROP INDEX` introspection for
  pre-existing exclusive-created indexes.
- Do not add SQL-level table-lock fault injection; prior exploratory SQL shapes
  did not reach the ownerless table-wait callback.

## Design

- Add an ownerless-only SQL policy predicate in
  `packages/libmylite/src/database.cc`.
- Reuse the existing SQL policy tokenizer and inspect only raw unquoted tokens,
  so string literals, comments, and quoted identifiers are not treated as
  special-index keywords.
- For ownerless read/write `CREATE` or `ALTER` statements, reject raw
  `FULLTEXT` or `SPATIAL` tokens before MariaDB prepares or executes the SQL.
- Keep ordinary exclusive embedded mode unchanged.
- Add a focused `special-index-policy` selector in
  `mylite_ownerless_cross_process_sql_test`.

## Compatibility Impact

Ownerless read/write mode explicitly does not support `FULLTEXT` or `SPATIAL`
index DDL. This preserves the existing ordinary secondary-index ownerless claim
while making special index classes deterministic unsupported surfaces until
their native storage, lock, and recovery behavior is designed.

Future support needs separate source-backed design for InnoDB full-text
auxiliary metadata, spatial R-tree page handling, spatial predicate locks,
special-index DDL recovery, and external oracle stress.

## Directory And Lifecycle Impact

The policy prevents ownerless mode from creating special index metadata or
special-index native storage in the MyLite database directory. It adds no new
files and verifies rejected objects remain absent across ownerless/native reopen
before and after forced shared-memory rebuild.

## Native Storage Impact

No native storage format changes. The slice only prevents ownerless entry into
unproven InnoDB full-text and spatial index paths.

## Binary Size And Dependencies

No binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `special-index-policy` selector.
- Build and run the focused `special-index-policy` selector in
  `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage, ownerless stress, and the
  hook ownerless SQL label, using focused reruns if the known intermittent
  InnoDB log-header checksum abort appears.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Ownerless special index DDL returns a MyLite policy error with MariaDB errno
  zero.
- Rejected `FULLTEXT`/`SPATIAL` index creation does not create index metadata.
- Rejected inline `FULLTEXT`/`SPATIAL` table definitions do not create
  application tables.
- The existing base table remains readable and survives ownerless/native reopen
  before and after forced `.shm` rebuild.
- Compatibility docs keep special-index ownerless support planned and mark the
  fail-closed policy boundary.

## Risks And Follow-Up

- Dropping pre-existing special indexes through ownerless mode still needs a
  metadata-aware policy or full special-index coordination design.
- Applications that require ownerless full-text or spatial search need a later
  compatibility slice with MariaDB comparison or external randomized stress.
