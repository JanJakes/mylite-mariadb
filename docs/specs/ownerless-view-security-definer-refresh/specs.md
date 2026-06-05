# Ownerless View Security Definer Refresh

## Problem Statement

Ownerless view coverage proves create/drop, replacement/alteration,
idempotent, and check-option refresh. The compatibility matrix still leaves view
security and definer metadata in the broader planned set. MyLite needs bounded
evidence that an already-open ownerless peer observes native MariaDB view
security metadata changes made by another ownerless process.

This slice adds deterministic SQL coverage for `DEFINER=CURRENT_USER`,
`SQL SECURITY DEFINER`, and `SQL SECURITY INVOKER` view definitions. It does not
claim privilege enforcement, account lifecycle, invalid-definer behavior, or
crash recovery during view security metadata rewrites.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_CREATE_VIEW` dispatches create, replace, and alter view execution
    through `mysql_create_view()`.
  - `SQLCOM_DROP_VIEW` dispatches view removal through `mysql_drop_view()`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` handles `VIEW_CREATE_NEW`, `VIEW_CREATE_OR_REPLACE`,
    and `VIEW_ALTER`.
  - `mysql_register_view()` stores the view definition file with the parsed
    definer and SQL-security metadata.
  - `mysql_drop_view()` deletes the native view definition and invalidates
    cached metadata.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats `CREATE`, `ALTER`, and
    `DROP` statements as dictionary-generation boundaries, so view security
    metadata changes should use the same already-open peer refresh path as
    other view definition changes.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - Existing view selectors cover simple create/drop, `CREATE OR REPLACE`,
    `ALTER VIEW`, idempotent no-ops, check-option semantics, and nested
    check-option refresh.

## Design

Add a focused ownerless SQL selector, `view-security-definer`.

The selector starts an ownerless parent handle and a child ownerless DDL process:

1. The child creates an InnoDB base table and a view with
   `DEFINER=CURRENT_USER SQL SECURITY DEFINER`.
2. The parent verifies the view file exists, `INFORMATION_SCHEMA.VIEWS` reports
   `SECURITY_TYPE='DEFINER'` with non-empty `DEFINER`, and the view query
   returns the expected rows through the already-open handle.
3. The child runs `CREATE OR REPLACE SQL SECURITY INVOKER VIEW` with a narrower
   predicate. The parent verifies `SECURITY_TYPE='INVOKER'` and the changed
   predicate.
4. The child runs `ALTER DEFINER=CURRENT_USER SQL SECURITY DEFINER VIEW` with a
   second predicate. The parent verifies security metadata returns to
   `DEFINER` and the altered predicate is visible.
5. The child drops the view. The parent verifies metadata and file absence while
   the base table remains durable.
6. Final assertions verify base-table rows and dropped-view absence through
   ownerless and ordinary exclusive reopen before and after forced `.shm`
   rebuild.

## Scope

In scope:

- Ownerless already-open peer refresh for view security metadata.
- Native MariaDB definer and `SECURITY_TYPE` metadata visibility.
- Ownerless/native reopen and forced `.shm` rebuild checks.
- Compatibility and cross-process-concurrency documentation updates.

Out of scope:

- MariaDB user/account lifecycle and privilege enforcement.
- Invalid or missing definers.
- `SQL SECURITY` crash injection.
- Stored functions, routines, and randomized view oracles.

## Compatibility Impact

No intended SQL behavior change. Ownerless mode continues to inherit MariaDB
view parsing and metadata behavior while MyLite coordinates dictionary refresh
for already-open peers. Compatibility remains partial for privilege/security
semantics and randomized view coverage.

## Directory And Lifecycle Impact

No directory layout changes. The view definition remains MariaDB-native metadata
under `datadir/<schema>/<view>.frm` while present. Final checks verify the file
is absent after `DROP VIEW` and the base table survives ownerless/native reopen
before and after volatile shared-memory rebuild.

## Native Storage Impact

The base table is InnoDB. The selector exercises native base-table reads and
writes while view security metadata changes, but it does not alter InnoDB file
formats, page-version replay, redo/checkpoint policy, or DDL file lifecycle.

## Binary Size Impact

Test and documentation only. No public API, production code path, or default
runtime feature is added.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test view-security-definer`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run adjacent view selectors and the ownerless SQL CTest shard containing the
  new selector.
- Run relevant ownerless DDL stress, `format-check`, `git diff --check`, and
  cached diff checks before commit.

## Acceptance Criteria

- An already-open ownerless peer sees `DEFINER` metadata for a view created by
  another ownerless process.
- The same peer sees replacement to `SQL SECURITY INVOKER`.
- The same peer sees alteration back to `SQL SECURITY DEFINER`.
- The same peer sees final `DROP VIEW`, and final view absence plus base-table
  rows survive ownerless/native reopen before and after forced `.shm` rebuild.
- Docs cross-link the hook-build security crash slice while keeping privilege
  enforcement, invalid definers, `ALTER DEFINER ... SQL SECURITY` crash
  injection, and randomized view coverage as planned.

## Risks And Follow-Up

- This proves metadata refresh for bounded security clauses, not user privilege
  enforcement.
- Hook-build crash recovery for definer creation and invoker replacement is
  covered by `docs/specs/ownerless-view-security-ddl-crash/specs.md`; invalid
  definer and `ALTER DEFINER ... SQL SECURITY` crash recovery remain separate
  planned work.
- Invalid-definer and randomized view-oracle coverage remain planned.
