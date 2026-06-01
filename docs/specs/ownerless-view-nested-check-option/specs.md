# Ownerless Nested View Check Option

## Problem Statement

Ownerless view check-option coverage proves `LOCAL` and `CASCADED` metadata and
valid/invalid DML for one updatable view over one InnoDB base table. It does
not yet prove MariaDB's nested-view distinction where an outer `LOCAL` check
option enforces only the outer predicate, while an outer `CASCADED` check
option also enforces underlying view predicates.

This matters for ownerless concurrency because nested views are durable
metadata files, while enforcement happens later through already-open peer
handles. A peer must refresh both the outer view and the underlying view
definition before allowing or rejecting DML through the nested view.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - `view_check_option` parses no option, `WITH CHECK OPTION`,
    `WITH CASCADED CHECK OPTION`, and `WITH LOCAL CHECK OPTION`.
- `mariadb/sql/sql_view.cc`
  - View creation records `with_check_option` in the native view definition
    file and rejects `WITH CHECK OPTION` for non-updatable views.
  - `mysql_make_view()` stores `TABLE_LIST::effective_with_check` through
    `LEX::get_effective_with_check()`.
- `mariadb/sql/sql_lex.cc`
  - `LEX::get_effective_with_check()` returns the view's stored `with_check`
    mode only for the applicable top-level view.
- `mariadb/sql/table.cc`
  - `TABLE_LIST::prep_check_option()` takes a `check_opt_type` parameter so an
    outer cascaded view can promote underlying views to `VIEW_CHECK_CASCADED`,
    while an outer local view passes `VIEW_CHECK_NONE` to underlying views.
  - `TABLE_LIST::view_check_option()` evaluates the prepared predicate and
    raises `ER_VIEW_CHECK_FAILED` on violation.
- `mariadb/sql/sql_insert.cc` and `mariadb/sql/sql_update.cc`
  - DML through an updatable view calls `view_check_option()` before applying
    the row change.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats `CREATE`, `ALTER`, and
    `DROP VIEW` statements as dictionary-generation boundaries, so already-open
    peers should refresh nested view metadata across these statements.

## Design

Add a focused selector, `view-nested-check-option`, to
`mylite_ownerless_cross_process_sql_test`.

The selector starts an ownerless parent handle and a child ownerless DDL
process:

1. The child creates an InnoDB base table, an inner view with
   `WHERE value >= 10 WITH CASCADED CHECK OPTION`, and an outer view over the
   inner view with `WHERE value <= 20 WITH LOCAL CHECK OPTION`.
2. The parent observes both native view files and both
   `INFORMATION_SCHEMA.VIEWS` rows. It inserts `value=5` through the outer view,
   proving outer `LOCAL` does not enforce the inner predicate, then verifies an
   outer-predicate violation still returns MariaDB errno 1369.
3. The child replaces the outer view with `WITH CASCADED CHECK OPTION`. The
   parent verifies the metadata change, rejects `value=6` through the outer
   view because the inner predicate is now cascaded, and inserts `value=15`.
4. The child alters the inner view to `WHERE value >= 8 WITH CASCADED CHECK
   OPTION` without changing the outer view. The parent verifies the already-open
   handle refreshes underlying view metadata by inserting `value=8` and
   rejecting `value=7`.
5. The child drops the outer and inner views. The parent verifies both views are
   absent while the base table still contains the rows inserted through the
   nested view.
6. Final assertions verify base-table rows plus view absence through ownerless
   and ordinary exclusive reopen before and after forced `.shm` rebuild.

## Scope

In scope:

- SQL-level ownerless coverage for nested updatable views.
- `LOCAL` versus `CASCADED` check-option propagation through an outer view.
- Already-open peer refresh for both outer and inner view definition changes.
- Check-option failure mapping to MariaDB errno 1369.
- Native view `.frm` file presence/absence inside the MyLite database
  directory.
- Compatibility and cross-process-concurrency documentation updates.

Out of scope:

- New production dictionary/storage code unless the selector exposes a bug.
- `SQL SECURITY`, definer/privilege behavior, invalid dependencies,
  non-updatable view diagnostics, prepared view DML, and routine interaction.
- Crash/fault injection during view definition rewrite.

## Compatibility Impact

No intended SQL behavior change. The slice expands ownerless compatibility
evidence for MariaDB nested updatable view check-option semantics while keeping
broader view semantics partial.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises native view metadata inside
the MyLite-owned database directory:

- `datadir/app/ownerless_view_nested_inner.frm` while the inner view exists,
- `datadir/app/ownerless_view_nested_outer.frm` while the outer view exists.

Final checks verify both files are absent after `DROP VIEW` and that the InnoDB
base table survives ownerless/native reopen before and after volatile
shared-memory rebuild.

## Native Storage Impact

The base table is InnoDB. The selector exercises native INSERT paths routed
through nested view metadata, but it does not change InnoDB file formats,
redo/page-version replay semantics, or page ownership rules.

## Binary Size Impact

Test and documentation only unless a bug fix is required. No dependency,
public API, or default runtime feature is added.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `view-nested-check-option` selector in `embedded-dev`.
- Run adjacent view selectors in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run adjacent view selectors in `ownerless-test-hooks`.
- Run the registered ownerless cross-process SQL CTest filters after
  implementation.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- An already-open ownerless peer sees nested view metadata created by another
  process.
- Outer `LOCAL` check option allows a row that satisfies the outer predicate
  while violating the inner predicate.
- Outer `CASCADED` check option rejects a row that violates the inner predicate.
- An already-open peer observes a later inner-view predicate change while the
  outer cascaded view remains in place.
- Invalid writes fail with MariaDB errno 1369.
- Final view absence and base-table state survive ownerless/native reopen
  before and after forced `.shm` rebuild.
- Docs continue to mark untested view security, dependency, prepared DML, and
  crash-recovery cases as planned.

## Risks And Open Questions

- This slice proves one nested merge-view shape. It does not cover complex
  joins, non-updatable views, `SQL SECURITY`, invalid dependencies, or prepared
  DML through nested views.
- Crash recovery during nested view file rewrite remains planned broader DDL
  recovery work.
