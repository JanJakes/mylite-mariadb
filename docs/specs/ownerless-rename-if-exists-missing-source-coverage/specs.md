# Ownerless Rename IF EXISTS Missing Source Coverage

## Problem Statement

Ownerless `RENAME TABLE IF EXISTS` coverage already proves existing-table
single-pair recovery, existing-table native-loop rollback, and deterministic
existing-table multi-pair rollback. A remaining documented gap is the
MariaDB-supported mixed list where some `IF EXISTS` sources are missing, those
pairs are skipped with warnings, and later existing-table pairs still execute.

This slice covers one bounded same-schema mixed list with a missing source
before and after one existing InnoDB source. It proves MariaDB warning/no-op
behavior, ownerless live-peer recovery after the successful mixed statement
reaches `dictionary-before-finish`, and native DDL-log rollback after the
existing pair reaches the `rename-table-after-native-file-op` hook.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8713-8724` parses
  `RENAME table_or_tables opt_if_exists ...` and records the optional
  `IF EXISTS` flag.
- `mariadb/sql/sql_parse.cc:4395-4406` dispatches `SQLCOM_RENAME_TABLE`,
  records session `OPTION_IF_EXISTS`, and calls
  `mysql_rename_tables(..., lex->if_exists())`.
- `mariadb/sql/sql_rename.cc:59-60` accepts the `if_exists` flag in
  `mysql_rename_tables()`.
- `mariadb/sql/sql_rename.cc:521-557` loops over rename pairs, calls
  `check_rename(..., skip_error || if_exists)`, continues when that check
  returns a skipped missing-source result, and calls `do_rename()` for existing
  pairs.
- `mariadb/mysql-test/main/rename.test:196-205` and
  `mariadb/mysql-test/main/rename.result:202-216` show the compatibility
  oracle: missing `IF EXISTS` sources emit note `1146`, the existing source is
  renamed, and a later target-exists conflict still errors.
- `packages/libmylite/src/database.cc:16818-16854` accepts optional
  `IF EXISTS` and comma-separated rename pairs in the ownerless dictionary
  recovery classifier; `database.cc:16856-16931` keeps view-only classification
  false when any mixed-list source is missing or not a view.

## Scope And Non-Goals

In scope:

- Add production ownerless SQL coverage for a mixed `RENAME TABLE IF EXISTS`
  list with missing-source no-ops surrounding one existing-table rename.
- Assert two MariaDB note `1146` warnings and verify only the existing source
  moved.
- Add hook-only live-peer recovery after the mixed list reaches
  `dictionary-before-finish`.
- Add hook-only native-loop rollback after the existing pair reaches
  `rename-table-after-native-file-op`.
- Verify native file-operation marker retention while a peer is live, final
  no-live marker drain, ownerless/native reopen, forced `.shm` rebuild, and
  file-level target absence for skipped pairs.

Out of scope:

- Arbitrary longer `IF EXISTS` rename-list permutations.
- Cross-schema, temporary/permanent, view-only, and foreign-key missing-source
  matrices.
- Target-exists conflict recovery after preceding missing-source warnings.
- Broader native redo/checkpoint reconciliation, DDL/file-lifecycle recovery,
  active-reader pressure oracle breadth, external MariaDB/RQG stress, and
  SQL-level local table-lock fault injection.

## Design

No production code change is intended. The existing ownerless recovery
classifier already parses `RENAME TABLE IF EXISTS` with comma-separated pairs,
and the view-only check treats a missing source as not-all-views, so the mixed
list remains on the base-table file-lifecycle recovery path.

The production test executes:

```sql
RENAME TABLE IF EXISTS
  app.ownerless_rename_if_exists_missing_before
    TO app.ownerless_rename_if_exists_missing_before_target,
  app.ownerless_rename_if_exists_missing_source
    TO app.ownerless_rename_if_exists_missing_target,
  app.ownerless_rename_if_exists_missing_after
    TO app.ownerless_rename_if_exists_missing_after_target
```

It verifies two note `1146` warnings, absence of skipped targets, presence and
durability of the moved target, ownerless reopen after forced `.shm` rebuild,
and ordinary native reopen.

The hook tests reuse the same SQL. The `dictionary-before-finish` selector
expects the existing source to have moved and the skipped targets to remain
absent. The `rename-table-after-native-file-op` selector expects MariaDB
DDL-log rollback to restore the original source table and original InnoDB
`SPACE` id while all targets remain absent.

## Compatibility Impact

SQL behavior stays MariaDB-owned. The slice adds evidence that MyLite exposes
MariaDB's mixed `RENAME TABLE IF EXISTS` no-op warning behavior in ownerless
mode and preserves the same storage result across crash recovery for the
covered mixed-list shape.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native storage format changes. The tests prove `.frm`
and `.ibd` files for skipped targets are never created, the moved or restored
table remains inside the MyLite-owned database directory, native file-operation
checkpoint markers remain durable while a peer is live, and no-live recovery
drains those markers.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive production change.
The implementation adds tests, CTest registration, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the production direct selector
  `rename-if-exists-missing-source-noop`.
- Run hook direct selectors
  `dictionary-rename-if-exists-missing-source-crash` and
  `dictionary-rename-if-exists-missing-source-loop-crash`.
- Run the adjacent registered hook CTest subset for existing-table and
  missing-source `IF EXISTS` rename crash coverage.
- Build the production embedded target and rerun the production direct
  selector.
- Run the production CI-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- The production selector observes exactly two note `1146` warnings, moves only
  the existing source, and leaves skipped targets absent.
- The prefinish crash selector recovers the moved table while a peer remains
  live, keeps skipped targets absent, and drains the native marker only after
  final no-live recovery.
- The native-loop crash selector rolls back to the original source table and
  original InnoDB `SPACE` id while all targets remain absent.
- Recovered state survives ownerless/native reopen and forced `.shm` rebuild.
- Compatibility docs no longer list the focused missing-source same-schema
  mixed `IF EXISTS` matrix as unproved.

## Risks And Follow-Up

- The slice proves one deterministic same-schema missing/existing/missing list.
  Cross-schema, mixed temporary/permanent, foreign-key, and arbitrary longer
  missing-source permutations remain completion work for this slice. A later
  target-conflict slice covers the reachable failed-DDL dictionary boundary
  and proves the native file-operation hook is not reached for errno `1050`.
- SQL-level local table-lock fault injection remains an unclaimed research
  path because supported SQL shapes still do not reach that callback.
