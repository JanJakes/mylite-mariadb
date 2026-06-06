# Ownerless View Prepared DML

## Problem Statement

Ownerless view check-option coverage proves direct SQL DML through updatable
views, including already-open peer refresh after view replacement and
alteration. Prepared view DML remained documented as a broader view gap.

Applications commonly use prepared statements for writes. Ownerless mode needs
evidence that prepared `INSERT` and `UPDATE` through an updatable view use the
same MariaDB check-option semantics and ownerless metadata refresh boundaries as
direct SQL.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_prepare.cc`
  - `mysql_stmt_execute_common()` runs prepared statements through
    `Prepared_statement::execute_loop()`.
  - Source comments state that prepared statement execution opens and locks
    tables the same way ordinary statements do.
  - Re-execution can reprepare a statement after metadata validation sees table
    or view changes.
- `mariadb/sql/sql_insert.cc`
  - Ordinary `INSERT` calls `TABLE_LIST::view_check_option()` before writing a
    row through an updatable view.
  - `INSERT ... ON DUPLICATE KEY UPDATE` also checks view options before the
    update branch writes the transformed row.
- `mariadb/sql/sql_update.cc`
  - `UPDATE` calls view check-option evaluation before applying updates through
    a view.
- `mariadb/sql/table.cc`
  - `TABLE_LIST::view_check_option()` raises `ER_VIEW_CHECK_FAILED` when the
    row does not satisfy the active view predicate.
- `packages/libmylite/src/database.cc`
  - `mylite_step()` wraps ownerless prepared execution with policy checks,
    ownerless statement locks, external page refresh, dictionary DDL finish,
    transaction state updates, and page-log reclamation before/after
    `mysql_stmt_execute()`.
  - Ordinary non-ownerless prepared execution still takes the direct
    `mysql_stmt_execute()` path.

## Design

Add a focused `view-prepared-check-option` selector to
`mylite_ownerless_cross_process_sql_test`.

The selector uses two ownerless read/write processes:

1. The parent opens the database before the view exists, proving it is an
   already-open peer.
2. A child creates an InnoDB base table and an updatable view with
   `WITH CASCADED CHECK OPTION`.
3. The parent prepares `SELECT`, `INSERT`, and `UPDATE` statements against the
   view, verifies valid prepared writes, and verifies invalid prepared writes
   fail with MariaDB errno 1369.
4. The child replaces the view with a narrower predicate and the same exposed
   column shape.
5. The parent reuses the already-prepared `INSERT` and `UPDATE` statements,
   proving ownerless refresh plus MariaDB reprepare/check-option semantics
   apply after peer view replacement.
6. The child drops the view, and final ownerless/native reopen checks before
   and after forced `.shm` rebuild verify view absence and base-table state.

## Scope

In scope:

- Prepared `SELECT` through an ownerless view after peer-created view metadata.
- Prepared `INSERT` through an updatable check-option view.
- Prepared `UPDATE` through an updatable check-option view.
- Reuse of prepared DML statements after a peer replaces the view predicate
  without changing the exposed column shape.
- MariaDB errno 1369 for invalid prepared insert/update attempts.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Prepared DDL, prepared `CREATE VIEW`, prepared `ALTER VIEW`, or prepared
  `DROP VIEW`.
- Prepared nested-view DML and complex join views.
- Non-updatable view diagnostics, invalid dependencies, invalid definers, view
  privilege/security semantics, routine interaction, and randomized view
  oracles.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving prepared view DML follows MariaDB updatable-view
check-option behavior and remains valid across ownerless peer view replacement.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises native MariaDB view `.frm`
metadata under `datadir/app/`, ownerless peer dictionary refresh, forced `.shm`
rebuild, and ordinary native exclusive reopen.

## Native Storage Impact

The base table is InnoDB. The slice verifies prepared DML through the view
updates native InnoDB rows and that rejected prepared DML leaves base-table state
unchanged. It does not change storage formats, redo/checkpoint policy, or
page-version WAL policy.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes. The
slice uses existing `mylite_prepare()`, `mylite_bind_*()`, `mylite_step()`,
`mylite_reset()`, and `mylite_finalize()` APIs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `view-prepared-check-option` selector in `embedded-dev`.
- Run adjacent view selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the relevant embedded ownerless SQL shard.
- Run `format-check`, Ubuntu 24.04 clang-format 18, `git diff --check`, and
  cached diff checks.

## Acceptance Criteria

- An already-open ownerless peer prepares statements against a view created by
  another ownerless process.
- Prepared valid insert/update writes through the view succeed and update the
  InnoDB base table.
- Prepared invalid insert/update attempts fail with MariaDB errno 1369 and do
  not mutate the base table.
- Reusing prepared DML after peer view replacement enforces the replacement
  predicate.
- Ownerless/native reopen before and after forced `.shm` rebuild observe final
  view absence and base-table rows.

## Risks And Follow-Up

- This covers one stable exposed-column shape across replacement. Prepared
  statements whose view column metadata changes remain broader view/prepared
  statement compatibility work.
- Prepared non-updatable diagnostics are covered separately by
  `docs/specs/ownerless-view-prepared-non-updatable-diagnostics/specs.md`.
- Prepared nested-view DML, invalid dependencies, invalid definers,
  privilege/security semantics, and randomized view oracles remain planned.
