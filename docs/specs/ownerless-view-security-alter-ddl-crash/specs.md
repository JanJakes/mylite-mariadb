# Ownerless View Security ALTER DDL Crash

## Problem Statement

Ownerless view security coverage proves already-open peers observe
`DEFINER=CURRENT_USER`, `SQL SECURITY DEFINER`, and `SQL SECURITY INVOKER`
metadata changes, and hook-build crash coverage proves definer view creation
plus invoker replacement. One bounded crash gap remains: a writer can die after
MariaDB rewrites a view through `ALTER DEFINER ... SQL SECURITY ... VIEW` but
before MyLite publishes ownerless dictionary finish.

This slice adds deterministic recovery evidence for that `ALTER VIEW` security
metadata boundary. The follow-up
[ownerless-view-security-live-recovery](../ownerless-view-security-live-recovery/specs.md)
promotes this focused `ALTER VIEW` security form to metadata-only live-peer
recovery.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_CREATE_VIEW` covers create, replace, and alter view execution via
    `mysql_create_view()`.
- `mariadb/sql/sql_view.cc`
  - `mysql_create_view()` handles `VIEW_ALTER`, processes definer and
    `SQL SECURITY` metadata, and registers the native view definition through
    `mysql_register_view()`.
  - `mysql_register_view()` writes the `.frm` definition file and removes stale
    table-definition-cache entries.
- `packages/libmylite/src/database.cc`
  - Ownerless dictionary DDL classification treats `ALTER VIEW` as a
    dictionary-generation boundary and exposes the unsafe
    `dictionary-before-finish` hook after native execution but before ownerless
    dictionary finish.

## Design

Add a focused unsafe-hook selector:

- `dictionary-view-security-alter-crash` initializes an InnoDB base table and
  an invoker view, kills a writer after
  `ALTER DEFINER=CURRENT_USER SQL SECURITY DEFINER VIEW ...` rewrites the
  native view definition but before ownerless dictionary finish, then verifies
  no-live recovery exposes `SECURITY_TYPE='DEFINER'`, non-empty `DEFINER`
  metadata, and the altered predicate.

The selector keeps another ownerless peer live, opens a new ownerless handle to
recover the completed metadata-only view rewrite, keeps the native
file-operation marker clear, then verifies ownerless and ordinary native reopen
before and after a forced `.shm` rebuild.

## Scope

In scope:

- Crash-at-`dictionary-before-finish` coverage for `ALTER DEFINER=CURRENT_USER
  SQL SECURITY DEFINER VIEW`.
- Recovered `.frm` presence under `datadir/app/`.
- Recovered `INFORMATION_SCHEMA.VIEWS.security_type` and non-empty `DEFINER`.
- View query behavior and base-table writes after recovery.
- Live-peer recovery without native file-operation marker evidence.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Invalid or missing definers.
- Privilege enforcement and account lifecycle behavior.
- Stored functions, routines, packages, and randomized view oracles.
- Broad view SQL-security crash matrices beyond this bounded `ALTER VIEW`
  spelling.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens ownerless compatibility
evidence by proving completed MariaDB `ALTER VIEW` security metadata rewrites
survive a writer death at MyLite's dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test exercises native MariaDB
view `.frm` files inside the MyLite-owned database directory, ownerless
live-peer recovery, final no-live cleanup, forced `.shm` rebuild, and native
exclusive reopen.

## Native Storage Impact

The base table is InnoDB. The slice verifies base-table durability and view
query behavior after recovery, but it does not alter InnoDB storage formats,
page-version replay policy, redo/checkpoint policy, or durable file-lifecycle
metadata.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run the focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-security-alter-crash`
- Run adjacent view security crash selectors:
  - `dictionary-view-security-create-crash`
  - `dictionary-view-security-replace-crash`
- Run the normal embedded `view-security-definer` selector.
- Run relevant ownerless hook/SQL subsets, format checks, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer remains open while a new ownerless opener recovers the completed
  security/definer view metadata.
- The native file-operation marker remains clear for this metadata-only view
  form.
- Recovery exposes `SECURITY_TYPE='DEFINER'`, non-empty `DEFINER` metadata,
  the `.frm` file, the altered predicate, and queryable view rows.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic `ALTER VIEW` security metadata recovery, not full
  SQL security semantics.
- Invalid-definer recovery, privilege enforcement, randomized view oracles, and
  external long-running DDL stress remain planned.
