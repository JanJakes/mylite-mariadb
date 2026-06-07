# Ownerless CREATE OR REPLACE Copy DDL Crash

## Problem Statement

Ownerless crash coverage already proves completed `CREATE TABLE ... LIKE`,
CTAS, and ordinary `CREATE OR REPLACE TABLE` native states survive a writer
death at MyLite's dictionary publication boundary. Stale-reader replay coverage
also covers `CREATE OR REPLACE TABLE ... LIKE` and
`CREATE OR REPLACE TABLE ... AS SELECT` final states.

The remaining bounded crash gap is the combined replacement-plus-copy shape:
when MariaDB drops an existing target and completes a LIKE-copy or CTAS
replacement before MyLite publishes ownerless dictionary finish, no-live
recovery must preserve the replacement copy as the durable final state.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:4709` through
  `mariadb/sql/sql_table.cc:4815` checks for existing targets. When
  `options.or_replace()` is set, MariaDB removes the old normal table with
  `mysql_rm_table_no_locks()` before continuing replacement creation.
- `mariadb/sql/sql_table.cc:5727` through
  `mariadb/sql/sql_table.cc:5862` implements `mysql_create_like_table()`: it
  opens the source and target metadata, rejects target/source self-replacement,
  copies the source description with `mysql_prepare_alter_table()`, clears
  inherited `DATA DIRECTORY` and `INDEX DIRECTORY`, preserves the statement's
  options including `OR REPLACE`, and creates the destination with
  `mysql_create_table_no_lock()`.
- `mariadb/sql/sql_table.cc:13417` through
  `mariadb/sql/sql_table.cc:13712` routes `CREATE TABLE ... LIKE` and CTAS
  through `Sql_cmd_create_table_like::execute()`. CTAS unlinks the target from
  the SELECT context and delegates to `select_create`; LIKE delegates to
  `mysql_create_like_table()`.
- `mariadb/sql/sql_insert.cc:4850` through
  `mariadb/sql/sql_insert.cc:4966` implements
  `select_create::create_table_from_items()`, deriving CTAS fields from the
  SELECT list and creating the destination through
  `mysql_create_table_no_lock()`.
- `mariadb/sql/sql_insert.cc:5377` through
  `mariadb/sql/sql_insert.cc:5412` finishes CTAS statement handling after
  rows are copied.
- `packages/libmylite/src/database.cc:8696` through
  `packages/libmylite/src/database.cc:8698` classifies `CREATE` as ownerless
  dictionary DDL.
- `packages/libmylite/src/database.cc:9278` through
  `packages/libmylite/src/database.cc:9328` begins ownerless dictionary DDL
  and exposes the unsafe `dictionary-before-finish` hook before ownerless
  dictionary finish is published.

## Design

Add two unsafe-hook selectors to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`:

- `dictionary-create-or-replace-like-crash` creates an old target table and a
  differently shaped source table with a secondary index, kills a writer after
  `CREATE OR REPLACE TABLE target LIKE source` completes natively but before
  ownerless dictionary finish, verifies live-peer cleanup remains busy, then
  verifies no-live recovery exposes the replacement copied definition, copied
  secondary index, empty replacement rowset, absent old metadata, native files,
  post-recovery writes, forced-index reads, and ownerless/native reopen before
  and after forced `.shm` rebuild.
- `dictionary-create-or-replace-ctas-crash` creates an old target table and a
  source table with rows, kills a writer after
  `CREATE OR REPLACE TABLE target ENGINE=InnoDB AS SELECT ...` completes
  natively but before ownerless dictionary finish, verifies live-peer cleanup
  remains busy, then verifies no-live recovery exposes the replacement CTAS
  definition, copied rows, absent old metadata, native files,
  post-recovery writes, and ownerless/native reopen before and after forced
  `.shm` rebuild.

The selectors reuse the existing `crash_dictionary_writer_with_live_peer()`,
`execute_sql_until_dictionary_fault()`, and reopen-oracle patterns. Production
code should not change unless these tests expose a defect.

## Scope And Non-Goals

In scope:

- crash-at-`dictionary-before-finish` coverage for representative
  `CREATE OR REPLACE TABLE ... LIKE` and
  `CREATE OR REPLACE TABLE ... ENGINE=InnoDB AS SELECT ...`,
- replacement copied-table presence in `INFORMATION_SCHEMA.TABLES`,
- replacement column metadata and old-column absence,
- replacement secondary-index metadata/use for the LIKE copy,
- replacement CTAS copied-row aggregates,
- native `.frm` and `.ibd` files under the MyLite-owned `datadir/app/`
  directory,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- crashes between old-target drop and replacement creation,
- partitioned sources, foreign-key or generated-column source shapes, special
  indexes, temporary-table copy DDL, storage-option variants, and unsupported
  native tablespace import/detach paths,
- SQL-level table-lock wait fault injection; prior explored SQL shapes stopped
  before the ownerless table-wait callback,
- external MariaDB/RQG long-running DDL stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless
`CREATE OR REPLACE TABLE ... LIKE` and CTAS replacement evidence by proving
completed native replacements survive writer death at MyLite's dictionary
publication boundary.

Full DDL/file-lifecycle recovery remains partial until durable lifecycle
metadata, broader native redo/checkpoint reconciliation, and external oracle
stress exist.

## DDL Metadata Routing Impact

The selectors use MariaDB's existing replacement, LIKE-copy, and CTAS routing
and MyLite's ownerless dictionary generation boundary. They verify the final
dictionary generation is recovered from completed native state when the writer
dies before publishing ownerless dictionary finish.

## Directory And Lifecycle Impact

No directory layout changes. The tests exercise MariaDB-native `.frm` and
InnoDB file-per-table `.ibd` replacement inside the MyLite database directory,
live-peer cleanup blocking, no-live ownerless recovery, forced `.shm` rebuild,
and ordinary native exclusive reopen.

## Native Storage Impact

No storage format changes. The old and replacement targets are ordinary InnoDB
tables. The slice does not change page-version replay policy, checkpoint
policy, or native file lifecycle metadata.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-like-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-or-replace-ctas-crash`
- Run adjacent hook selectors:
  - `dictionary-create-like-crash`
  - `dictionary-ctas-crash`
  - `dictionary-create-or-replace-table-crash`
- Run relevant hygiene checks: `format-check`, `git diff --check`, and
  temp/process cleanup checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- `CREATE OR REPLACE TABLE ... LIKE` recovery exposes the copied replacement
  through `INFORMATION_SCHEMA.TABLES`, preserves copied columns and secondary
  index metadata, removes old columns/indexes, starts with an empty replacement
  rowset, accepts post-recovery rows, and supports forced-index reads.
- `CREATE OR REPLACE TABLE ... AS SELECT` recovery exposes the replacement
  table and columns, removes old columns, preserves copied rows, and accepts
  post-recovery writes.
- Both replacement targets have native `.frm` and `.ibd` files under
  `datadir/app/`.
- Ownerless and ordinary native reopen observe the same replacement states
  before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic representative crash coverage, not an exhaustive
  replacement-copy DDL matrix.
- Crashes between native old-table removal and replacement creation remain a
  separate lower-level DDL recovery problem.
- Broader DDL/file lifecycle recovery and external MariaDB/RQG stress remain
  planned.
