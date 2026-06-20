# Ownerless CREATE OR REPLACE TABLE Crash

## Problem Statement

Ownerless table-idempotent DDL coverage already proves an already-open peer can
observe `CREATE OR REPLACE TABLE` replacing an existing InnoDB table
definition. The crash boundary still needs focused evidence: if a writer dies
after MariaDB drops the old native table and creates the replacement table but
before MyLite publishes ownerless dictionary finish, no-live recovery must
preserve the replacement table as the durable final state.

This slice adds deterministic hook-build crash recovery evidence for
representative `CREATE OR REPLACE TABLE` replacement.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:2050` through
  `mariadb/sql/sql_table.cc:2085` documents the failed
  `CREATE OR REPLACE TABLE` cleanup path, where MariaDB logs a generated
  `DROP TABLE IF EXISTS` when the original table was dropped but replacement
  creation failed.
- `mariadb/sql/sql_table.cc:4710` through
  `mariadb/sql/sql_table.cc:4808` checks for an existing target table. When
  `options.or_replace()` is set, MariaDB deletes existing table statistics and
  removes the old normal table through `mysql_rm_table_no_locks()` before
  continuing replacement creation.
- `mariadb/sql/sql_table.cc:5292` through
  `mariadb/sql/sql_table.cc:5295` creates the replacement table through
  `mysql_create_table_no_lock()`.
- `mariadb/sql/sql_table.cc:5300` through
  `mariadb/sql/sql_table.cc:5323` handles the locked-tables reconnect path for
  `CREATE OR REPLACE TABLE`; MyLite ownerless SQL rejects locked-table mode, so
  this slice covers ordinary ownerless replacement.
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

Add an unsafe-hook selector,
`dictionary-create-or-replace-table-crash`, to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

The selector:

- creates and populates an original InnoDB table with old columns and an old
  secondary index,
- verifies the original `.frm`/`.ibd` files and old metadata are present,
- keeps a live ownerless peer open,
- kills a writer after `CREATE OR REPLACE TABLE` completes natively but before
  ownerless dictionary finish,
- verifies live-peer cleanup remains busy until no-live recovery,
- verifies the replacement table exists with the new columns, new secondary
  index, absent old columns/index, empty replacement rowset, and native
  `.frm`/`.ibd` files,
- writes rows through the replacement table and checks forced-index reads,
- verifies ownerless and ordinary native reopen before and after forced `.shm`
  rebuild.

## Scope And Non-Goals

In scope:

- crash-at-`dictionary-before-finish` coverage for representative
  `CREATE OR REPLACE TABLE` over an existing InnoDB table,
- recovered replacement table definition and empty replacement rowset,
- recovered old-column and old-index absence,
- recovered replacement secondary-index metadata and post-recovery DML,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- replacement of tables with foreign keys, triggers, generated columns,
  partitioning, special indexes, or unsupported storage options,
- broader failed replacement cleanup variants beyond the representative
  old-table-removal boundary covered by
  `docs/specs/ownerless-create-or-replace-after-drop-crash/specs.md`,
- SQL locked-table mode, which ownerless SQL rejects,
- external MariaDB/RQG long-running DDL stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless
`CREATE OR REPLACE TABLE` compatibility evidence by proving the completed
replacement survives writer death at MyLite's dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test exercises MariaDB-native
`.frm` and InnoDB file-per-table `.ibd` replacement under the MyLite-owned
database directory, live-peer cleanup blocking, no-live ownerless recovery,
forced `.shm` rebuild, and ordinary native exclusive reopen.

## Native Storage Impact

The replaced and replacement tables are ordinary InnoDB tables. The slice does
not change InnoDB storage formats, page-version replay policy, or checkpoint
policy.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-table-crash`
- Run the normal embedded `table-idempotent-ddl` selector, which covers
  already-open peer refresh for replacement.
- Run the hook crash-tail selector or the relevant ownerless hook SQL shard.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovery exposes the replacement table through `INFORMATION_SCHEMA.TABLES`.
- Old columns and old secondary-index metadata are absent.
- New columns and new secondary-index metadata are present.
- The replacement table starts empty, accepts post-recovery rows, and supports
  forced-index reads.
- Ownerless and ordinary native reopen observe the same replacement state
  before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This covers a representative successful replacement, not every replacement
  interaction with constraints, triggers, generated columns, or partitioning.
- A follow-up after-drop crash slice covers the representative lower-level
  old-table-removal boundary; copy variants and constraint-heavy replacements
  remain broader DDL lifecycle coverage.
- Broader DDL/file lifecycle recovery and external MariaDB/RQG stress remain
  planned.
