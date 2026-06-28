# Ownerless Rename IF EXISTS Live Recovery

## Problem Statement

Ownerless rename-list recovery already covers explicit schema-qualified
`RENAME TABLE` lists, implicit-schema rename, and `ALTER TABLE ... RENAME TO`.
MariaDB also accepts `RENAME TABLE IF EXISTS ... TO ...`. Before this slice,
the ownerless dictionary recovery classifier expected the first rename source
immediately after `RENAME TABLE`, so a writer killed after native rename work
for the `IF EXISTS` form could not use the focused live-peer recovery lane.

This slice adds bounded live-peer crash recovery evidence for an existing-table
`RENAME TABLE IF EXISTS` statement. It proves that MyLite recognizes the
MariaDB token shape, recovers the native file move while another ownerless peer
remains live, retains the native file-operation marker until no-live checkpoint
proof drains it, and preserves the target table through ownerless/native reopen
and forced shared-memory rebuild.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8713-8725` parses
  `RENAME table_or_tables opt_if_exists ...` and records the option in
  `LEX::create_info`.
- `mariadb/sql/sql_parse.cc:4397-4405` handles `SQLCOM_RENAME_TABLE`, maps the
  session option into `DDL_options_st::OPT_IF_EXISTS`, and calls
  `mysql_rename_tables(..., lex->if_exists())`.
- `mariadb/sql/sql_rename.cc:56-181` implements `mysql_rename_tables()`, and
  `mariadb/sql/sql_rename.cc:506-674` implements the ordered
  `rename_tables()` pair loop used by ordinary rename lists.
- `mariadb/sql/sql_table.cc:5562-5700` performs native table/file rename work
  through `mysql_rename_table()`.
- `packages/libmylite/src/database.cc` classifies completed ownerless
  dictionary DDL for recoverable live-peer cleanup after native file-operation
  redo was observed.

## Scope And Non-Goals

In scope:

- Accept optional `IF EXISTS` immediately after `RENAME TABLE` in the ownerless
  rename recovery classifier.
- Keep source-view preflight aligned with the same optional tokens.
- Add hook-build crash coverage for an existing-table
  `RENAME TABLE IF EXISTS app.source TO app.target` killed at
  `dictionary-before-finish` while another ownerless peer remains live.
- Verify live marker retention, final no-live marker drain, ownerless/native
  reopen, and forced `.shm` rebuild.

Out of scope:

- Missing-source `RENAME TABLE IF EXISTS` warnings and no-op semantics.
- Crash injection inside MariaDB's native rename loop or DDL-log rollback path.
- Exhaustive mixed `IF EXISTS` rename lists, view-only `IF EXISTS` rename, and
  temporary-table `IF EXISTS` rename variants.
- Partitioned-table rename support.

## Design

Add a small token helper for optional `IF EXISTS` and use it in
`ownerless_rename_table_recovery_statement()` before consuming rename pairs.
`ownerless_rename_table_sources_are_views()` skips the same optional tokens so
existing metadata-only view-rename classification does not regress.

The test adds `dictionary-rename-if-exists-crash` to
`mylite_ownerless_cross_process_sql_test`. It creates an InnoDB source table,
kills a writer after MariaDB completes:

```sql
RENAME TABLE IF EXISTS app.ownerless_rename_if_exists_crash_source
TO app.ownerless_rename_if_exists_crash_target
```

and before MyLite finishes ownerless dictionary publication. A live ownerless
peer remains open during recovery. The recovering opener verifies the source is
absent, the target is present and writable, and the native file-operation marker
remains durable until the live peer exits.

## Compatibility Impact

Successful SQL behavior remains MariaDB-owned and unchanged. Ownerless crash
recovery evidence expands to a MariaDB-supported `RENAME TABLE IF EXISTS`
syntax for existing InnoDB tables. Missing-table no-op warning behavior remains
MariaDB-owned and is not claimed as a file-lifecycle recovery boundary here.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native storage format changes. The slice uses MariaDB's
native `.frm`/`.ibd` rename work inside the MyLite-owned database directory,
the existing ownerless native file-operation marker, and existing no-live
checkpoint drain behavior.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive production change.
The implementation changes first-party ownerless SQL classification plus hook
test coverage.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run direct selector `dictionary-rename-if-exists-crash`.
- Run adjacent rename crash CTests:
  `dictionary-rename-file-op-marker-crash`,
  `dictionary-rename-if-exists-crash`,
  `dictionary-implicit-rename-crash`, and
  `dictionary-alter-table-rename-crash`.
- Run focused production ownerless rename refresh/replay selectors where
  relevant.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- A writer killed after native `RENAME TABLE IF EXISTS` succeeds but before
  ownerless dictionary finish can be recovered while another ownerless peer
  remains live.
- The source table is absent, the target table is present and writable, and the
  target rows survive ownerless/native reopen and forced `.shm` rebuild.
- The native file-operation marker remains set while the live peer is open and
  drains after final no-live recovery.

## Risks And Follow-Up

- This is a positive existing-table recovery boundary, not coverage for
  missing-source `IF EXISTS` warning paths.
- Mixed multi-pair `IF EXISTS`, view-only `IF EXISTS`, temporary-table rename,
  and native-loop crash injection remain broader DDL/file-lifecycle follow-up
  work.
