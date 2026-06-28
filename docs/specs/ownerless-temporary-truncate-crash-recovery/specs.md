# Ownerless Temporary Truncate Crash Recovery

## Problem Statement

Ownerless temporary-table tracking covers normal `TRUNCATE TABLE` semantics, but
the ownerless dictionary recovery classifier still routes a tracked temporary
truncate through the generic durable-table truncate recovery kind. That can make
the hook crash boundary look like a durable native file operation even though
MariaDB resolved and truncated only the connection-local temporary table.

MyLite must classify `TRUNCATE [TABLE]` on a tracked temporary table as
temporary-table metadata-only recovery, then prove a crash before ownerless
dictionary finish preserves the permanent table and drains cleanly after the
last live peer exits.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/temporary_tables.cc:115-138` resolves temporary tables from the
  connection-local temporary table list.
- `mariadb/sql/sql_truncate.cc:510-532` performs truncate/recreate after SQL
  resolution and locking; when the resolved table is temporary, the permanent
  table with the same name is not the target.
- `packages/libmylite/src/database.cc:15672-15733` chooses the ownerless
  dictionary recovery kind, checking temporary-table recovery before generic
  `TRUNCATE TABLE` recovery.
- `packages/libmylite/src/database.cc:19302-19325` treats
  `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_TEMPORARY_TABLE` as metadata-only for
  ownerless recovery and native-file checkpoint policy.
- `packages/libmylite/src/database.cc:19390-19555` currently recognizes
  temporary create/drop/rename recovery, but not tracked temporary truncate.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  dictionary-before-finish temporary DROP/RENAME hook selectors that keep a live
  ownerless peer open while the crashed writer is recovered.

## Design

Add tracked temporary truncate recognition to
`ownerless_temporary_table_recovery_statement()`.

Accepted forms:

- `TRUNCATE tracked_temp`
- `TRUNCATE TABLE tracked_temp`
- the same forms with an optional schema-qualified table name
- trailing semicolons

The classifier must require the resolved table token to match a handle-local
tracked temporary table name. It must not classify untracked permanent truncates
as temporary recovery.

Add a hook-only selector named `temporary-truncate-crash` and CTest
`libmylite.ownerless-temporary-truncate-crash`. The selector:

1. Creates a permanent InnoDB table with the target name and durable rows.
2. Starts a writer child that opens ownerless, creates a same-named temporary
   table, inserts temporary rows, arms `dictionary-before-finish`, runs
   `TRUNCATE TABLE`, and is killed by the hook.
3. Keeps another ownerless peer live while the killed writer is recovered.
4. Requires the native file-operation checkpoint marker to remain retained while
   a peer is live, matching the existing temporary DROP/RENAME recovery lane,
   and to drain after final no-live recovery.
5. Verifies the permanent table rows remain visible and writable through a live
   ownerless peer, final ownerless reopen, forced `.shm` rebuild, and ordinary
   native reopen.

## Scope And Non-Goals

In scope:

- Classifying tracked temporary-table truncate as temporary metadata recovery.
- Hook crash coverage at the ownerless dictionary-before-finish boundary.
- A same-name temporary/permanent shadowing case with permanent-table
  preservation.

Out of scope:

- Arbitrary native mid-truncate crashes inside MariaDB/InnoDB.
- Multi-table temporary DDL matrices.
- Partitioned temporary tables.
- Positive durable permanent-table truncate recovery, which is already covered
  by durable-table truncate selectors.

## Compatibility Impact

The change aligns ownerless recovery metadata with MariaDB temporary-table
semantics. User-visible SQL behavior is unchanged: temporary truncate remains
session-local and the durable permanent table is not truncated.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format change. The new classifier prevents a
temporary-table truncate from claiming a durable native file-operation recovery
kind. Like the existing temporary DROP/RENAME recovery lane, the native
file-operation checkpoint marker can remain retained while a live peer exists
and must drain after final no-live recovery.

## Public API, Build, Size, License

No public API, dependency, binary-size, or license changes. The slice changes
first-party classifier logic, adds one hook selector/CTest, and updates docs.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run direct selector `temporary-truncate-crash`.
- Run focused CTest `libmylite.ownerless-temporary-truncate-crash`.
- Run adjacent temporary crash CTests for drop, rename, alter-rename, and mixed
  rename coverage.
- Build `php-embedded-prod` SQL test and rerun the production normal
  `temporary-table-truncate-tracking` selector.
- Run ownerless temporary stress smoke, format check, CI production-build
  audit, and diff whitespace checks.

## Acceptance Criteria

- A tracked temporary `TRUNCATE TABLE` selects
  `MYLITE_OWNERLESS_DICTIONARY_RECOVERY_TEMPORARY_TABLE` recovery.
- The hook crash selector retains the native file-operation checkpoint marker
  while a peer is live and drains it after final no-live recovery.
- The permanent table with the same name is not truncated by the killed
  temporary truncate.
- Ownerless/native reopen and forced `.shm` rebuild preserve the permanent
  table and allow follow-up writes.

## Risks And Follow-Up

- Native mid-truncate crashes remain outside this hook boundary.
- Broader temporary DDL and randomized external DDL oracle stress remain
  planned.
