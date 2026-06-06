# Ownerless View Idempotent DDL Crash

## Problem

Ownerless view-idempotent DDL coverage verifies peer refresh for
`CREATE VIEW IF NOT EXISTS` and `DROP VIEW IF EXISTS`, including duplicate
create and missing-drop no-op behavior. Ordinary hook-build view crash coverage
kills mutating simple view create/drop writers, but the duplicate-create and
missing-drop no-op paths still need deterministic crash evidence.

This slice adds hook-build recovery evidence for duplicate idempotent view
create and missing idempotent view drop.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `CREATE VIEW` with `opt_if_not_exists`
  before `table_ident` and `DROP VIEW` with `opt_if_exists`.
- `mariadb/sql/sql_view.cc` `mysql_create_view()` treats an existing view under
  `IF NOT EXISTS` as success with diagnostics and preserves the existing view
  definition, while plain duplicate `CREATE VIEW` raises errno 1050.
- `mariadb/sql/sql_table.cc` `mysql_rm_table_no_locks()` treats missing
  objects under `IF EXISTS` as successful no-op drops with diagnostics.
- `packages/libmylite/src/database.cc`
  `ownerless_dictionary_ddl_statement()` classifies `CREATE` and `DROP` as
  ownerless dictionary DDL and exposes the unsafe `dictionary-before-finish`
  hook after native SQL execution but before ownerless dictionary finish.

## Design

Add two unsafe-hook selectors to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`:

- `dictionary-view-idempotent-create-crash` creates an InnoDB base table and a
  view using `CREATE VIEW IF NOT EXISTS`, then kills a writer after duplicate
  `CREATE VIEW IF NOT EXISTS` for the same view name with a different
  definition reaches `dictionary-before-finish`.
- `dictionary-view-idempotent-drop-crash` creates an InnoDB base table and a
  real view, then kills a writer after `DROP VIEW IF EXISTS` for a missing view
  name reaches `dictionary-before-finish`.

Both selectors keep a live ownerless peer open while the writer is killed,
prove cleanup remains busy until no-live recovery, then verify ownerless and
ordinary native reopen before and after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Crash-at-`dictionary-before-finish` coverage for duplicate
  `CREATE VIEW IF NOT EXISTS` no-op behavior.
- Crash-at-`dictionary-before-finish` coverage for missing
  `DROP VIEW IF EXISTS` no-op behavior.
- Definition preservation for the real view.
- Absence of missing-view native files and metadata after no-op drop recovery.
- Ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `CREATE OR REPLACE VIEW` and `ALTER VIEW` crash recovery, which is covered
  by `docs/specs/ownerless-view-replacement-ddl-crash/specs.md`.
- Nested-view, security/definer, invalid-dependency, and updatable-view crash
  variants.
- Explicit column-list crash coverage, which is covered separately by
  `docs/specs/ownerless-view-column-list-ddl-crash/specs.md`.
- Check-option create/replacement crash coverage, which is covered separately by
  `docs/specs/ownerless-view-check-option-ddl-crash/specs.md`, and
  check-option alter crash coverage, which is covered by
  `docs/specs/ownerless-view-check-option-alter-ddl-crash/specs.md`.
- Trigger, routine, and table-lock crash variants.
- External randomized DDL/RQG stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless evidence for
MariaDB-compatible idempotent view DDL by proving a dead writer in MyLite's
dictionary publication window does not replace the existing view definition,
drop the real view, create missing-view metadata, or leave stale peer state.

## Directory And Lifecycle Impact

No directory layout changes. The tests exercise native MariaDB view `.frm`
files under `datadir/app/`, ownerless live-peer cleanup blocking, no-live
recovery, forced `.shm` rebuild, and ordinary native exclusive reopen.

## Native Storage Impact

The base tables are InnoDB. The views are MariaDB SQL-layer metadata. MyLite
does not reinterpret the view definition; it coordinates the ownerless
dictionary boundary and verifies durable reopen behavior for the preserved
native metadata.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-idempotent-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-idempotent-drop-crash`
- Run normal `view-idempotent-ddl` selectors in `embedded-dev` and
  `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest shards, `format-check`, and diff checks.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- Duplicate idempotent create recovery keeps the original view definition,
  leaves the attempted replacement definition absent, and keeps plain duplicate
  create returning errno 1050.
- Missing idempotent drop recovery keeps the real view present and queryable
  while the missing view remains absent.
- Ownerless and ordinary native reopen observe the same rows and metadata
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This covers deterministic no-op view DDL crash recovery, not every view
  lifecycle spelling.
- Replacement/alter, explicit column-list, check-option create/replacement/alter,
  nested check-option, security/definer, and selected trigger/view variants are
  covered separately; other nested variants, invalid dependency, and broader
  randomized DDL oracle execution remain planned.
