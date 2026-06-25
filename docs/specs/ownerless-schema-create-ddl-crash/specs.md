# Ownerless Schema Create DDL Crash

## Problem Statement

Ownerless schema lifecycle coverage proves already-open peers observe
`CREATE DATABASE`, qualified InnoDB table use inside the new schema, and
`DROP DATABASE`. Hook coverage already kills `DROP DATABASE` after native
schema/table removal but before ownerless dictionary finish. The symmetric
create boundary still needs focused evidence: if a writer dies after MariaDB
creates the native schema directory and `db.opt` file but before MyLite
publishes ownerless dictionary finish, no-live recovery must preserve the
created schema as durable state.

This slice adds deterministic hook-build crash recovery evidence for
representative `CREATE DATABASE`.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_parse.cc:5145` through
  `mariadb/sql/sql_parse.cc:5160` dispatches `SQLCOM_CREATE_DB` to
  `mysql_create_db()` after access checks and charset/collation resolution.
- `mariadb/sql/sql_db.cc:747` through `mariadb/sql/sql_db.cc:818`
  implements `mysql_create_db_internal()`. It locks the schema name, builds the
  native schema directory path, creates that directory with `my_mkdir()`, then
  writes the native schema option file through `write_db_opt()`.
- `packages/libmylite/src/database.cc:8694` through
  `packages/libmylite/src/database.cc:8697` classifies `CREATE` statements as
  ownerless dictionary DDL.
- `packages/libmylite/src/database.cc:9276` through
  `packages/libmylite/src/database.cc:9318` begins ownerless dictionary DDL,
  and `packages/libmylite/src/database.cc:9321` through
  `packages/libmylite/src/database.cc:9345` exposes the unsafe
  `dictionary-before-finish` hook before ownerless dictionary finish is
  published.

## Design

Add an unsafe-hook selector, `dictionary-schema-create-crash`, to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

The selector:

- verifies `ownerless_schema_create_crash` is absent,
- keeps a live ownerless peer open,
- kills a writer after `CREATE DATABASE ownerless_schema_create_crash DEFAULT
  CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci` completes natively but
  before ownerless dictionary finish,
- verifies live-peer recovery with the native file-operation marker clear,
- verifies the recovered schema directory and `db.opt` file exist,
- verifies `INFORMATION_SCHEMA.SCHEMATA` reports the recovered defaults,
- creates and writes an InnoDB table in the recovered schema,
- verifies ownerless and ordinary native reopen before and after forced `.shm`
  rebuild.

## Scope And Non-Goals

In scope:

- crash-at-`dictionary-before-finish` coverage for representative
  `CREATE DATABASE`,
- recovered native schema directory and `db.opt` file,
- recovered schema metadata and explicit charset/collation defaults,
- post-recovery InnoDB table creation and DML inside the recovered schema,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `CREATE OR REPLACE DATABASE`, idempotent duplicate-create crash variants,
  schema rename, and invalid charset/collation failure paths,
- schema create followed by table creation in the same interrupted statement,
  which MariaDB does not provide as one SQL statement,
- external MariaDB/RQG long-running DDL stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless
schema compatibility evidence by proving completed native schema creation
survives writer death at MyLite's dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test exercises the MariaDB
native schema directory and `db.opt` under the MyLite-owned `datadir/`
directory, ownerless live-peer recovery, forced `.shm` rebuild, and ordinary
native exclusive reopen.

## Native Storage Impact

The schema metadata is MariaDB-native SQL-layer metadata. The post-recovery
table is an ordinary InnoDB table created after recovery to prove the recovered
schema can own native table files.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-schema-create-crash`
- Run the normal embedded `schema-lifecycle` selector, which covers already-open
  peer refresh for schema create/drop.
- Run the hook crash-tail selector or the relevant ownerless hook SQL shard.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- Live-peer recovery observes the schema while the native file-operation marker
  remains clear.
- Recovery exposes the schema through `INFORMATION_SCHEMA.SCHEMATA`.
- The schema directory and `db.opt` file exist under `datadir/`.
- Explicit recovered charset/collation defaults are visible.
- An InnoDB table can be created and written inside the recovered schema.
- Ownerless and ordinary native reopen observe the same schema/table state
  before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This covers representative successful schema creation, not the full schema
  DDL matrix.
- Failure cleanup for invalid schema-create options remains a separate slice.
- Broader DDL/file lifecycle recovery and external MariaDB/RQG stress remain
  planned.
