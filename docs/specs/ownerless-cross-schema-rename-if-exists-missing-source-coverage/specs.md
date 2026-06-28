# Ownerless Cross-Schema Rename IF EXISTS Missing Source Coverage

## Problem Statement

The same-schema missing-source `RENAME TABLE IF EXISTS` slice proves skipped
source warning/no-op semantics around one existing InnoDB rename. A remaining
DDL/file-lifecycle gap is the same mixed list when the existing table moves
across schema directories, because recovery must preserve both MariaDB's
warning behavior and the native `.frm`/`.ibd` move between database
subdirectories.

This slice covers a bounded `app` to `app_archive` mixed list with a missing
source before the cross-schema move and a missing source after it.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8713-8724` parses
  `RENAME table_or_tables opt_if_exists ...`.
- `mariadb/sql/sql_parse.cc:4395-4406` calls
  `mysql_rename_tables(..., lex->if_exists())`.
- `mariadb/sql/sql_rename.cc:521-557` skips missing sources when
  `if_exists` is active and still executes `do_rename()` for existing pairs.
- `mariadb/mysql-test/main/rename.test:196-205` and
  `mariadb/mysql-test/main/rename.result:202-216` prove note `1146` warnings
  for skipped pairs and a successful existing rename in the same statement.
- `packages/libmylite/src/database.cc:16818-16931` already accepts optional
  `IF EXISTS` comma-list syntax and keeps mixed missing/base-table sources on
  the base-table recovery path.

## Scope And Non-Goals

In scope:

- Add production ownerless SQL coverage for a mixed cross-schema
  `RENAME TABLE IF EXISTS` list with skipped sources around one existing
  InnoDB move from `app` to `app_archive`.
- Add hook-only live-peer recovery at `dictionary-before-finish`.
- Add hook-only native-loop rollback at `rename-table-after-native-file-op`.
- Verify skipped targets are absent, the moved or restored table has the
  expected rows, the original `SPACE` id is preserved on rollback, and
  ownerless/native reopen plus forced `.shm` rebuild keep the recovered state.

Out of scope:

- Foreign-key, temporary/permanent, view-only, target-conflict, and arbitrary
  longer missing-source permutations.
- Broader native redo/checkpoint reconciliation, active-reader pressure oracle
  breadth, external MariaDB/RQG stress, and SQL-level local table-lock fault
  injection.

## Design

No production code change is required. The tests reuse the existing rename
recovery classifier and unsafe native file-operation hook.

The statement shape is:

```sql
RENAME TABLE IF EXISTS
  app.ownerless_cross_schema_rename_if_exists_missing_before
    TO app.ownerless_cross_schema_rename_if_exists_missing_before_target,
  app.ownerless_cross_schema_rename_if_exists_missing_source
    TO app_archive.ownerless_cross_schema_rename_if_exists_missing_target,
  app_archive.ownerless_cross_schema_rename_if_exists_missing_after
    TO app_archive.ownerless_cross_schema_rename_if_exists_missing_after_target
```

The production selector validates the two note `1146` warnings, target schema
placement, skipped-target absence, forced `.shm` rebuild, and native reopen.
The prefinish crash selector expects the cross-schema target to be present and
writable while skipped targets remain absent. The native-loop selector expects
MariaDB DDL-log rollback to restore the source under `app`, keep
`app_archive` present, remove all targets, and preserve the original source
`SPACE` id.

## Compatibility Impact

SQL behavior remains MariaDB-owned. The slice adds ownerless evidence that
MariaDB's mixed missing-source `RENAME TABLE IF EXISTS` behavior is preserved
when the existing pair crosses schema directories and when crash recovery
replays or rolls back the native file lifecycle.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or storage-format changes. The tests prove the target
`.frm`/`.ibd` files live only under `app_archive` after completed recovery and
that rollback restores source files under `app` while skipped targets are not
created in either schema directory.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive production change.
The implementation adds tests, CTest registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run direct selectors:
  `cross-schema-rename-if-exists-missing-source-noop`,
  `dictionary-cross-schema-rename-if-exists-missing-source-crash`, and
  `dictionary-cross-schema-rename-if-exists-missing-source-loop-crash`.
- Run the registered hook CTest subset for same-schema and cross-schema
  missing-source IF EXISTS rename crash coverage.
- Build the production embedded target and rerun the production cross-schema
  selector.
- Run production CI-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- The production selector observes the skipped-source warnings and the
  cross-schema target table only under `app_archive`.
- The prefinish crash selector recovers the moved cross-schema target while a
  peer remains live and drains the native marker only after no-live recovery.
- The native-loop crash selector restores the original source table, original
  `SPACE` id, and source files under `app`, with all targets absent.
- Recovered state survives ownerless/native reopen and forced `.shm` rebuild.

## Risks And Follow-Up

- This closes a focused cross-schema missing/existing/missing matrix. FK,
  temporary/permanent, target-conflict, and randomized longer-list matrices
  remain open.
