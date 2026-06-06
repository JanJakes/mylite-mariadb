# Ownerless View Non-Updatable Diagnostics

## Problem Statement

Ownerless view coverage now proves create/query/drop, definition replacement,
explicit column lists, `WITH CHECK OPTION`, nested check-option propagation,
prepared DML through one updatable check-option view, and security metadata
refresh. Non-updatable view diagnostics remain a documented broader view gap.

Applications depend on stable MariaDB/MySQL diagnostics when they accidentally
write through aggregate, temporary-table, or otherwise non-updatable views.
Ownerless mode needs evidence that an already-open peer refreshes
non-updatable view metadata from another process and returns MariaDB-compatible
errors without mutating the native InnoDB base table.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` records `TABLE_LIST::updatable_view` from merge
    eligibility and rejects `WITH CHECK OPTION` when the view is not updatable.
  - `ER_VIEW_NONUPD_CHECK` is MariaDB errno 1368.
- `mariadb/sql/table.cc`
  - `TABLE_LIST::single_table_updatable()` returns false when the view or a
    single underlying view is marked non-updatable.
- `mariadb/sql/sql_insert.cc`
  - INSERT preparation rejects non-updatable view targets with
    `ER_NON_INSERTABLE_TABLE`, MariaDB errno 1471.
- `mariadb/sql/sql_update.cc`
  - UPDATE preparation rejects non-updatable view targets with
    `ER_NON_UPDATABLE_TABLE`, MariaDB errno 1288.
- `mariadb/sql/sql_delete.cc`
  - DELETE preparation rejects non-updatable view targets with
    `ER_NON_UPDATABLE_TABLE`, MariaDB errno 1288.
- `packages/libmylite/src/database.cc`
  - Ownerless direct SQL execution runs through the same statement gate,
    dictionary refresh, DDL finish, transaction-state, and page-log cleanup
    path as the covered view DDL/DML selectors.

## Design

Add a focused `view-non-updatable-diagnostics` selector to
`mylite_ownerless_cross_process_sql_test`.

The selector uses two ownerless read/write processes:

1. The parent opens before the view exists, proving it is an already-open peer.
2. A child creates an InnoDB base table and a non-updatable aggregate view.
3. The parent verifies `INFORMATION_SCHEMA.VIEWS.IS_UPDATABLE = 'NO'`, queries
   the view, and verifies `INSERT`, `UPDATE`, and `DELETE` through the view
   fail with MariaDB errnos 1471 and 1288 without changing the base table.
4. The parent attempts `CREATE OR REPLACE VIEW ... WITH CASCADED CHECK OPTION`
   over a non-updatable aggregate definition and verifies errno 1368 with no
   durable view-definition replacement.
5. The child replaces the view with a different non-updatable aggregate
   projection.
6. The parent observes the replacement metadata/results, rechecks non-updatable
   DML diagnostics, and verifies the base table remains unchanged.
7. The child drops the view, and final ownerless/native reopen checks before
   and after forced `.shm` rebuild verify view absence and base-table state.

## Scope

In scope:

- Already-open ownerless peer refresh for a non-updatable view created by
  another ownerless process.
- `IS_UPDATABLE = 'NO'` metadata after create and replacement.
- Direct SQL `INSERT`, `UPDATE`, and `DELETE` diagnostics through the
  non-updatable view.
- MariaDB errno 1471 for non-insertable view INSERT.
- MariaDB errno 1288 for non-updatable view UPDATE and DELETE.
- MariaDB errno 1368 for `WITH CHECK OPTION` on a non-updatable replacement.
- Base-table immutability across failed view writes.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Prepared DML diagnostics for non-updatable views.
- Crash-at-boundary recovery for non-updatable view DDL.
- Complex join, union, algorithm-specific, privilege/security, invalid-definer,
  invalid-dependency, and randomized view oracle coverage.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving MariaDB non-updatable view metadata and DML diagnostics are
preserved across ownerless peer refresh and durable reopen.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises native MariaDB view `.frm`
metadata under `datadir/app/`, peer dictionary refresh, failed DDL with no
durable replacement, final `DROP VIEW`, ownerless/native reopen, and forced
`.shm` rebuild.

## Native Storage Impact

The base table is InnoDB. Failed non-updatable view DML must not mutate native
InnoDB rows. The slice does not change storage formats, redo/checkpoint policy,
page-version WAL policy, or DDL file-operation recovery.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes. The
slice uses existing direct SQL execution and MariaDB diagnostics exposed through
`mylite_mariadb_errno()`.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `view-non-updatable-diagnostics` selector in `embedded-dev`.
- Run adjacent view selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the relevant embedded ownerless SQL shard.
- Run `format-check`, Ubuntu 24.04 clang-format 18, `git diff --check`, and
  cached diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes a non-updatable view created by
  another ownerless process.
- View metadata reports `IS_UPDATABLE = 'NO'` after create and replacement.
- Direct INSERT through the view fails with errno 1471.
- Direct UPDATE and DELETE through the view fail with errno 1288.
- `WITH CHECK OPTION` on a non-updatable replacement fails with errno 1368 and
  preserves the original view definition.
- Failed view writes do not mutate the native InnoDB base table.
- Final view absence and base-table rows survive ownerless/native reopen before
  and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers direct SQL diagnostics for representative aggregate non-updatable
  views. Prepared diagnostics, joins, unions, algorithm-specific cases,
  invalid dependencies, invalid definers, privilege/security semantics,
  crash-boundary recovery, and randomized view oracles remain planned.
