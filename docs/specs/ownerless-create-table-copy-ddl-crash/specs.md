# Ownerless CREATE TABLE Copy DDL Crash

## Problem Statement

Ownerless peer-refresh coverage already exercises `CREATE TABLE ... LIKE` and
`CREATE TABLE ... SELECT` from another process. The crash boundary still needs
focused evidence: if a writer dies after MariaDB creates the native destination
table but before MyLite publishes ownerless dictionary finish, no-live recovery
must preserve the completed native table-copy state.

This slice adds deterministic hook-build crash recovery evidence for
representative `CREATE TABLE ... LIKE` and CTAS (`CREATE TABLE ... SELECT`)
DDL.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:5727` through
  `mariadb/sql/sql_table.cc:5862` implements
  `mysql_create_like_table()`. It opens the source and target metadata locks,
  copies the source table description through `mysql_prepare_alter_table()`,
  explicitly avoids inheriting `DATA DIRECTORY` and `INDEX DIRECTORY`, and
  creates the destination table through `mysql_create_table_no_lock()`.
- `mariadb/sql/sql_insert.cc:4850` through
  `mariadb/sql/sql_insert.cc:4966` implements
  `select_create::create_table_from_items()`. It derives destination columns
  from selected items, validates the create fields, and creates the CTAS
  destination through `mysql_create_table_no_lock()`.
- `mariadb/sql/sql_insert.cc:5377` through
  `mariadb/sql/sql_insert.cc:5410` implements `select_create::send_eof()`,
  where CTAS completes statement-final logging and commit handling after rows
  are copied.
- `packages/libmylite/src/database.cc:8694` through
  `packages/libmylite/src/database.cc:8697` classifies `CREATE` statements as
  ownerless dictionary DDL, so `CREATE TABLE ... LIKE` and CTAS run through the
  dictionary-generation protocol.
- `packages/libmylite/src/database.cc:9276` through
  `packages/libmylite/src/database.cc:9318` begins ownerless dictionary DDL
  and exposes the unsafe `dictionary-after-begin` hook.
- `packages/libmylite/src/database.cc:9321` through
  `packages/libmylite/src/database.cc:9345` exposes the unsafe
  `dictionary-before-finish` hook before ownerless dictionary finish is
  published. Killing a writer at this hook after SQL execution tests recovery
  of the completed native MariaDB state.

## Design

Add two unsafe-hook selectors to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`:

- `dictionary-create-like-crash` creates an InnoDB source table with a
  secondary index, verifies the destination table and native files are absent,
  keeps a live ownerless peer open, kills a writer after the
  `CREATE TABLE ... LIKE` copy completes natively but before ownerless
  dictionary finish, verifies live-peer cleanup remains busy, then reopens
  with no live peer and checks the copied columns, index metadata,
  `.frm`/`.ibd` files, empty destination semantics, post-recovery writes, and
  forced-index reads.
- `dictionary-ctas-crash` creates an InnoDB source table with rows, verifies
  the destination table and native files are absent, keeps a live ownerless
  peer open, kills a writer after
  `CREATE TABLE app.ownerless_ctas_crash_copy ENGINE=InnoDB AS SELECT ...`
  completes natively but before ownerless dictionary finish, verifies live-peer
  cleanup remains busy, then reopens with no live peer and checks the created
  columns, copied rows, `.frm`/`.ibd` files, post-recovery writes, and final
  ownerless/native visibility.

Both selectors verify ownerless and ordinary native reopen before and after a
forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- crash-at-`dictionary-before-finish` coverage for representative
  `CREATE TABLE ... LIKE` and CTAS DDL,
- recovered destination table presence in `INFORMATION_SCHEMA.TABLES`,
- recovered `INFORMATION_SCHEMA.COLUMNS` metadata,
- recovered secondary-index metadata for the `LIKE` copy,
- recovered `.frm` and `.ibd` files under the MyLite-owned `datadir/app/`
  directory,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `CREATE OR REPLACE TABLE ... LIKE`, `CREATE TABLE IF NOT EXISTS ... LIKE`,
  temporary-table copy DDL, partitioned sources, special indexes, generated
  column copy variants, foreign-key copy variants, and CTAS conflict/error
  paths,
- SQL-level table-lock fault injection for native table-wait paths,
- external MariaDB/RQG long-running DDL stress.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless
`CREATE TABLE ... LIKE` and CTAS compatibility evidence by proving completed
native destination tables survive writer death at MyLite's dictionary
publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The tests exercise MariaDB-native
`.frm` and InnoDB file-per-table `.ibd` files inside the MyLite database
directory, ownerless process cleanup while a live peer remains open, no-live
dictionary recovery, forced `.shm` rebuild, and ordinary native exclusive
reopen.

## Native Storage Impact

The destination tables are ordinary InnoDB tables created by MariaDB. The slice
does not change InnoDB storage formats, page-version replay policy, or
checkpoint policy.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-create-like-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-ctas-crash`
- Run the normal embedded `ddl-broader` selector, which already covers
  live-peer visibility for `CREATE TABLE ... LIKE` and CTAS.
- Run the hook crash-tail selector or the relevant ownerless hook SQL shard.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- `CREATE TABLE ... LIKE` recovery exposes the copied table through
  `INFORMATION_SCHEMA.TABLES`, preserves the copied column and secondary-index
  metadata, keeps the destination empty until post-recovery inserts, and allows
  forced-index reads.
- CTAS recovery exposes the created table and columns through
  `INFORMATION_SCHEMA`, preserves copied rows, and allows post-recovery writes.
- Both destination tables have native `.frm` and `.ibd` files under
  `datadir/app/`.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic representative crash coverage, not an exhaustive
  table-copy DDL matrix.
- SQL-level table-lock fault injection remains unproven for explored SQL
  shapes and is not assumed reachable by this slice.
- Broader DDL/file lifecycle recovery and external MariaDB/RQG stress remain
  planned work.
