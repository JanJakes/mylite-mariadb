# Ownerless View Invalid Dependency Diagnostics

## Problem Statement

Ownerless view coverage now proves create/query/drop, replacement, explicit
column lists, check-option behavior, nested check-option propagation, prepared
check-option DML, non-updatable diagnostics, and security metadata refresh. It
still needs focused evidence for a MariaDB-native invalid view caused by a peer
dropping and later recreating the view's base table.

Applications can keep a view definition after a dependency disappears. MariaDB
then reports errno 1356 for queries through the invalid view until the
dependency is repaired or the view is dropped. Ownerless mode needs evidence
that an already-open peer observes that invalid transition, then refreshes back
to a valid view when the base table is recreated.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/libmariadb/include/mysqld_error.h`
  - `ER_VIEW_INVALID` is MariaDB errno 1356.
- `mariadb/sql/sql_base.cc`
  - When opening a table that belongs to a view fails because the dependency is
    missing, `open_table()` clears the lower-level error and raises
    `ER_VIEW_INVALID` for the owning view.
- `mariadb/sql/table.cc`
  - `TABLE_LIST::replace_view_error_with_generic()` converts dependency lookup
    failures such as `ER_NO_SUCH_TABLE` into `ER_VIEW_INVALID`.
- `mariadb/sql/sql_derived.cc`
  - Derived/view preparation hides selected underlying view-resolution errors
    behind `ER_VIEW_INVALID`.
- `packages/libmylite/src/database.cc`
  - `ownerless_dictionary_ddl_statement()` classifies `CREATE` and `DROP`
    statements as ownerless dictionary DDL.
  - `ownerless_begin_dictionary_ddl()` and `ownerless_finish_dictionary_ddl()`
    publish dictionary-generation boundaries for ownerless DDL.
  - `refresh_ownerless_dictionary_before_statement()` waits for a ready
    generation and flushes table/dictionary state before the already-open peer
    executes a later statement.

## Design

Add a focused `view-invalid-dependency` selector to
`mylite_ownerless_cross_process_sql_test`.

The selector uses two ownerless read/write processes:

1. The parent opens before the view exists, proving it is an already-open peer.
2. A child creates an InnoDB base table and a simple view over that table.
3. The parent observes the view metadata, native `.frm` file, base `.ibd` file,
   and valid query results through the view.
4. The child drops the base table, leaving the view definition file present.
5. The parent observes the missing base `.ibd`, sees the view still listed in
   `INFORMATION_SCHEMA.VIEWS`, and verifies querying the view fails with
   MariaDB errno 1356.
6. The child recreates the base table with new rows under the same name.
7. The parent verifies the same already-open handle refreshes back to valid view
   results and sees the recreated base-table rows.
8. The child drops the view, and final ownerless/native reopen checks before and
   after forced `.shm` rebuild verify view absence and recreated base-table
   state.

## Scope

In scope:

- Already-open ownerless peer refresh for view dependencies changed by another
  ownerless process.
- MariaDB errno 1356 after the base table is dropped while the view definition
  remains.
- Recovery of the same view definition after the dependency is recreated under
  the same schema/table name.
- Final ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Invalid definers, privilege/security failures, stored functions, trigger
  dependencies, nested invalid dependencies, and complex join/union views.
- Crash-at-boundary recovery for dropping an already-invalid view is covered by
  `docs/specs/ownerless-view-invalid-dependency-drop-live-recovery/specs.md`.
- External randomized view oracle coverage.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving a MariaDB-native invalid-view diagnostic and repair sequence
is preserved across ownerless peer dictionary refresh and durable reopen.

## Directory And Lifecycle Impact

No directory layout changes. The selector checks native MariaDB view `.frm`
metadata and InnoDB `.ibd` base-table state under `datadir/app/`, then verifies
ownerless/native reopen before and after forced `.shm` rebuild.

## Native Storage Impact

The base table is InnoDB. The slice exercises native table drop/recreate around
a retained view definition but does not change storage formats,
redo/checkpoint policy, page-version WAL policy, or DDL file-operation
recovery.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes. The
selector uses existing direct SQL execution and MariaDB diagnostics exposed
through `mylite_mariadb_errno()`.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `view-invalid-dependency` selector in `embedded-dev`.
- Run adjacent view selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the relevant embedded ownerless SQL shard.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes a view created by another ownerless
  process.
- Dropping the base table leaves the view metadata visible but causes direct
  query through the view to fail with errno 1356.
- Recreating the base table under the same name makes the same view queryable
  again from the already-open peer.
- Final view absence and recreated base-table rows survive ownerless/native
  reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers one direct SQL dependency-drop/recreate shape. Invalid definers,
  privilege/security failures, nested invalid dependencies, complex views,
  and randomized view oracles remain planned. Crash-boundary recovery for
  dropping an already-invalid view is covered separately.
