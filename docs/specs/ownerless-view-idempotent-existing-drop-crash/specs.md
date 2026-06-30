# Ownerless View Existing IF EXISTS Drop Crash Recovery

## Problem

Ownerless view-idempotent crash coverage proves duplicate
`CREATE VIEW IF NOT EXISTS` and missing `DROP VIEW IF EXISTS` no-op behavior.
MariaDB also accepts `DROP VIEW IF EXISTS` when the view exists; that branch is
not a no-op, but it uses idempotent syntax and removes the native view metadata.

This slice adds focused live-peer recovery evidence for the mutating
`DROP VIEW IF EXISTS <existing view>` branch.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `DROP VIEW` with `opt_if_exists` before
  the view identifier.
- `mariadb/sql/sql_table.cc` `mysql_rm_table_no_locks()` handles `DROP VIEW`
  and applies `IF EXISTS` to missing-object diagnostics while existing objects
  still enter the normal removal path.
- `packages/libmylite/src/database.cc`
  `ownerless_drop_view_recovery_statement()` consumes optional `IF EXISTS` and
  classifies a single-view drop as metadata-only ownerless dictionary recovery.

## Design

Add the hook-build selector
`dictionary-view-idempotent-existing-drop-crash`.

The test creates an InnoDB base table plus an existing view, then kills a child
writer after:

```sql
DROP VIEW IF EXISTS app.ownerless_view_idempotent_existing_drop_crash
```

reaches `dictionary-before-finish`. The parent keeps another ownerless peer
live, verifies the removed view while the native file-operation marker remains
clear, releases the peer, and verifies ownerless/native reopen before and after
forced shared-memory rebuild.

## Compatibility Impact

No SQL syntax or public API changes. The slice broadens ownerless recovery
evidence for MariaDB-compatible idempotent view drop syntax by proving the
existing-object removal branch, not just the missing-object no-op branch.

## Storage And Lifecycle Impact

The native view `.frm` metadata file under `datadir/app/` is removed. This is a
metadata-only view operation, so the native file-operation checkpoint marker
must remain clear.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-dictionary-view-idempotent-(create|drop|existing-drop)-crash$' --output-on-failure`.
- Run the adjacent view crash selector group.
- Run relevant production embedded view/ownerless peer-refresh cases.
- Run `tools/check-ci-production-builds`,
  `cmake --build --preset format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- A killed `DROP VIEW IF EXISTS` writer for an existing view recovers while
  another ownerless peer remains live.
- The view `.frm` file and `INFORMATION_SCHEMA.VIEWS` row are absent after
  recovery.
- Queries against the dropped view fail while the base table remains durable and
  writable.
- The native file-operation marker remains clear.
- Ownerless/native reopen and forced `.shm` rebuild preserve the absent-view
  state and base-table durability.

## Non-Goals

- Multi-view `DROP VIEW` lists.
- Invalid dependency, privilege, or definer matrices.
- External MariaDB/RQG randomized stress.
