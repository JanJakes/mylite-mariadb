# Ownerless View Prepared Non-Updatable Diagnostics

## Problem Statement

Direct SQL ownerless coverage now proves non-updatable aggregate-view metadata
refresh and MariaDB diagnostics for `INSERT`, `UPDATE`, `DELETE`, and rejected
`WITH CHECK OPTION` definitions. Prepared statements remained explicitly
planned for non-updatable view diagnostics.

Applications commonly issue writes through prepared statements. Ownerless mode
needs evidence that prepared DML targeting a non-updatable view fails during
MariaDB prepare-time analysis with the same MariaDB errno surface and without
mutating native InnoDB base rows.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_prepare.cc`
  - `Prepared_statement::prepare()` parses with
    `CONTEXT_ANALYSIS_ONLY_PREPARE`, then calls `check_prepared_statement()`
    before marking the statement prepared.
  - Failed prepare-time analysis rolls back metadata locks to the prepare
    savepoint and never reaches statement execution.
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
  - `mylite_prepare()` exposes MariaDB prepared-statement diagnostics through
    the existing MyLite/MariaDB error APIs.

## Design

Extend the existing `view-non-updatable-diagnostics` selector in
`mylite_ownerless_cross_process_sql_test`.

The selector already uses an already-open ownerless parent and a child ownerless
DDL process to create, replace, and drop a non-updatable aggregate view. Add
prepare-time assertions to both the original view definition and the replaced
view definition:

1. `mylite_prepare()` for `INSERT` through the non-updatable view fails with
   MariaDB errno 1471.
2. `mylite_prepare()` for `UPDATE` through the non-updatable view fails with
   MariaDB errno 1288.
3. `mylite_prepare()` for `DELETE` through the non-updatable view fails with
   MariaDB errno 1288.
4. The existing base-table immutability and reopen checks continue to prove no
   rejected prepared DML mutates native InnoDB rows.

## Scope

In scope:

- Prepared `INSERT`, `UPDATE`, and `DELETE` diagnostics against the existing
  representative aggregate non-updatable view.
- Already-open peer refresh before prepared diagnostics.
- Rechecking prepared diagnostics after peer view replacement.
- Base-table immutability after rejected prepare attempts.

Out of scope:

- Prepared diagnostics for nested, join, union, or algorithm-specific
  non-updatable views.
- Prepared DDL or prepared `WITH CHECK OPTION` replacement attempts.
- Crash-at-boundary recovery for prepared non-updatable diagnostics.
- Invalid dependencies, invalid definers, privilege/security semantics, and
  randomized view oracles.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving prepared non-updatable view DML exposes MariaDB prepare-time
diagnostics through the MyLite API and leaves base-table state unchanged.

## Directory And Lifecycle Impact

No directory layout changes. The selector reuses native MariaDB view `.frm`
metadata under `datadir/app/`, peer dictionary refresh, final `DROP VIEW`,
ownerless/native reopen, and forced `.shm` rebuild from the direct diagnostics
slice.

## Native Storage Impact

The base table is InnoDB. Rejected prepared statements must not mutate native
InnoDB rows. The slice does not change storage formats, redo/checkpoint policy,
page-version WAL policy, or DDL file-operation recovery.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes. The
slice adds test coverage around existing `mylite_prepare()` diagnostics.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `view-non-updatable-diagnostics` selector in `embedded-dev`.
- Run adjacent view selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the relevant embedded ownerless SQL shard.
- Run `format-check`, Ubuntu 24.04 clang-format 18, `git diff --check`, and
  cached diff checks.

## Acceptance Criteria

- Prepared INSERT through the non-updatable view fails during prepare with
  errno 1471.
- Prepared UPDATE and DELETE through the non-updatable view fail during prepare
  with errno 1288.
- The same prepared diagnostics hold after a peer replaces the non-updatable
  view definition.
- Failed prepare attempts do not mutate the native InnoDB base table.
- Existing ownerless/native reopen checks still observe final view absence and
  base-table rows.

## Risks And Follow-Up

- This covers one aggregate non-updatable view shape. Prepared diagnostics for
  nested, join, union, algorithm-specific, invalid-dependency, invalid-definer,
  privilege/security, and randomized view-oracle cases remain planned.
