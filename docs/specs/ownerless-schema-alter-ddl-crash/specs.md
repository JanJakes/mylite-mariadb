# Ownerless Schema Alter DDL Crash

## Problem

Ownerless schema-default coverage verifies `CREATE DATABASE ... DEFAULT
CHARACTER SET/COLLATE`, peer-visible `ALTER DATABASE`, inherited table column
collations, native `db.opt` presence, and schema drop. Schema-create and
schema-drop crash coverage now protect the create/remove dictionary boundaries,
but an interrupted `ALTER DATABASE` `db.opt` rewrite remained untested.

This slice adds hook-build crash recovery evidence for a writer killed after
MariaDB rewrites the native schema option file but before MyLite publishes
ownerless dictionary finish.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_ALTER_DB` to
  `mysql_alter_db()`.
- `mariadb/sql/sql_db.cc` `mysql_alter_db_internal()` locks the schema name and
  rewrites `MY_DB_OPT_FILE` (`db.opt`) with the new default charset/collation.
- `mariadb/sql/sql_show.cc` exposes schema defaults through
  `INFORMATION_SCHEMA.SCHEMATA`.
- `packages/libmylite/src/database.cc` classifies leading `ALTER` statements
  as ownerless dictionary DDL and exposes the unsafe
  `dictionary-before-finish` hook after native SQL execution but before
  ownerless dictionary finish.

## Design

Add an unsafe-hook selector, `dictionary-schema-alter-crash`, to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

The selector:

- creates `ownerless_schema_alter_crash` with `latin1` defaults,
- creates and writes an InnoDB table whose `VARCHAR` column inherits `latin1`,
- keeps a live ownerless peer open,
- kills a writer after `ALTER DATABASE ownerless_schema_alter_crash DEFAULT
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci` completes natively but
  before ownerless dictionary finish,
- verifies live-peer recovery with the native file-operation marker clear,
- verifies recovered schema defaults are `utf8mb4`/`utf8mb4_unicode_ci`,
- verifies the pre-alter table keeps its `latin1` column metadata,
- creates a post-recovery table that inherits the recovered `utf8mb4` defaults,
  and
- verifies ownerless/native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for representative
  `ALTER DATABASE ... DEFAULT CHARACTER SET/COLLATE`.
- Recovered native schema directory and `db.opt`.
- Pre-alter and post-alter table column collation checks.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Invalid charset/collation failure cleanup.
- Schema rename, which MariaDB handles through separate upgrade paths.
- Broader durable file-lifecycle metadata for every DDL class.
- External MariaDB/RQG long-running DDL stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless
schema-default compatibility evidence by proving a completed native
`ALTER DATABASE` option-file rewrite survives writer death at MyLite's
dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises MariaDB's native
`datadir/<schema>/db.opt`, ownerless live-peer recovery, forced `.shm` rebuild,
and ordinary native exclusive reopen.

## Native Storage Impact

The schema option file is MariaDB-native SQL-layer metadata. The test creates
ordinary InnoDB tables before and after recovery to prove inherited defaults
and native table storage remain usable.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-alter-crash`
- Run adjacent schema selectors in `embedded-dev` and `ownerless-test-hooks`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- Live-peer recovery observes the altered defaults while the native
  file-operation marker remains clear.
- Recovery exposes the altered schema defaults through
  `INFORMATION_SCHEMA.SCHEMATA`.
- The schema directory and `db.opt` file exist under `datadir/`.
- The pre-alter table keeps its original `latin1` column metadata.
- A post-recovery table inherits the altered `utf8mb4` defaults.
- Ownerless and ordinary native reopen observe the same schema/table state
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers representative successful schema-default rewrite, not every
  invalid option or warning path.
- Broader DDL/file lifecycle recovery and external MariaDB/RQG stress remain
  planned.
