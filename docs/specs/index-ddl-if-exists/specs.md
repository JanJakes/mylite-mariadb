# Index DDL IF EXISTS

## Goal

Cover MariaDB-compatible existence options for routed index DDL.
`CREATE INDEX IF NOT EXISTS` should skip existing indexes with warnings and
create missing supported indexes through the MyLite copy-rebuild path.
`DROP INDEX IF EXISTS` should skip missing indexes with warnings and drop
existing supported indexes without corrupting rows or other indexes.
`ALTER TABLE ... ADD INDEX IF NOT EXISTS` and `ALTER TABLE ... DROP INDEX IF
EXISTS` should provide the same skip/create/drop behavior through ALTER-table
syntax.
`ALTER TABLE ... RENAME INDEX IF EXISTS` should skip missing indexes with
warnings and rename existing supported indexes through the same copy-rebuild
path.

## Non-Goals

- Do not exhaust every `ALTER TABLE ADD/DROP INDEX IF [NOT] EXISTS` spelling;
  one representative secondary-index add/drop path is the target.
- Do not cover `ALTER INDEX IF EXISTS ... [NOT] IGNORED`; index ignorability
  affects optimizer-visible metadata and is covered by
  [Index Ignorability](../index-ignorability/specs.md).
- Do not add online/in-place index DDL; MyLite still forces supported index
  DDL through copy rebuilds.
- Do not add FULLTEXT, SPATIAL, unbounded BLOB/TEXT unique, foreign-key, or
  partition index support.
- Do not change storage format, public API, or index-entry page layout.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_yacc.yy:2615-2633` parses standalone
  `CREATE INDEX opt_if_not_exists` and routes it through `add_create_index()`.
- `mariadb/sql/sql_yacc.yy:2634-2645` does the same for standalone
  `CREATE UNIQUE INDEX opt_if_not_exists`.
- `mariadb/sql/sql_yacc.yy:7976-7985` parses `ALTER TABLE ... ADD INDEX IF
  NOT EXISTS` through `ADD key_def`.
- `mariadb/sql/sql_yacc.yy:8073-8082` parses `ALTER TABLE ... DROP INDEX IF
  EXISTS` into an `Alter_drop::KEY`.
- `mariadb/sql/sql_yacc.yy:13431-13448` parses standalone
  `DROP INDEX opt_if_exists_table_element` and stores an `Alter_drop::KEY`.
- `mariadb/sql/sql_yacc.yy:8121-8131` parses
  `ALTER TABLE ... RENAME INDEX IF EXISTS ... TO ...` into the index rename
  alter list.
- `mariadb/sql/sql_table.cc:6248-6266` describes
  `handle_if_exists_options()`, which removes skipped create/drop items from
  ALTER metadata before execution.
- `mariadb/sql/sql_table.cc:6386-6512` removes missing `DROP ... IF EXISTS`
  key operations and pushes note-level diagnostics.
- `mariadb/sql/sql_table.cc:6514-6542` removes missing
  `RENAME INDEX IF EXISTS` operations and pushes note-level diagnostics.
- `mariadb/sql/sql_table.cc:6566-6665` removes duplicate
  `ADD KEY IF NOT EXISTS` requests and pushes note-level diagnostics.

## Compatibility Impact

This slice covers partial MariaDB compatibility for standalone routed index
DDL existence options:

- duplicate supported index creates are warning-producing no-ops;
- missing supported index creates add MyLite catalog/index metadata through
  the existing copy-rebuild path;
- representative ALTER-table index add forms match standalone create skip and
  add semantics;
- missing index drops are warning-producing no-ops;
- existing index drops remove index metadata while preserving rows and other
  supported indexes;
- representative ALTER-table index drop forms match standalone drop skip and
  drop semantics;
- missing index renames are warning-producing no-ops;
- existing index renames update index metadata while preserving rows and index
  entries;
- close/reopen discovery sees the committed index state.

The behavior remains partial because exhaustive ALTER-table index spellings,
online/in-place index algorithms, unsupported index classes, foreign keys, and
partitions remain separate work.

## Design

No production code change is expected. MariaDB maps standalone index DDL to
ALTER metadata, handles the existence options, and then invokes the same
copy-rebuild paths MyLite already supports for standalone `CREATE INDEX` and
`DROP INDEX`, representative ALTER-table add/drop index forms, and SQL-level
index rename.

The MyLite-specific risk is index metadata publication. The test must prove
that skipped duplicate creates leave the existing index usable, new creates
publish forced-index-readable entries, representative ALTER-table add forms do
the same, skipped missing drops do not mutate the table, representative
ALTER-table drop forms do the same, skipped missing renames do not mutate the
table, existing renames move forced-index access to the new name, and existing
drops remove the index from reopened metadata.

## Affected Subsystems

- MariaDB parser and ALTER preparation determine existence-option semantics.
- MyLite copy-rebuild DDL publishes supported index metadata and rebuilt index
  entries.
- MyLite warning capture exposes MariaDB diagnostics after successful direct
  execution.
- MyLite statement checkpoints protect file-backed index DDL publication paths.

## DDL Metadata Routing Impact

Supported standalone and ALTER-table index creates/drops plus SQL-level index
renames still rebuild the table definition inside the MyLite catalog. Skipped
existence-option operations do not publish a new definition.

## Single-File And Lifecycle Impact

Successful index creates/drops/renames and skipped existence-option statements
must not create persistent `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`,
`aria_log.*`, binlog, relay-log, or plugin-owned table sidecars. After final
close, the MyLite runtime directory must be empty.

## Public API, File Format, Build, And Dependency Impact

There are no public API, file-format, build-profile, binary-size, license, or
dependency changes.

## Test And Verification Plan

1. Add storage-engine smoke coverage for `CREATE INDEX IF NOT EXISTS` skipping
   an existing supported secondary index.
2. Add storage-engine smoke coverage for `CREATE INDEX IF NOT EXISTS` creating
   a missing supported secondary index.
3. Add storage-engine smoke coverage for `DROP INDEX IF EXISTS` skipping a
   missing index.
4. Add storage-engine smoke coverage for `DROP INDEX IF EXISTS` dropping an
   existing supported secondary index.
5. Add storage-engine smoke coverage for `RENAME INDEX IF EXISTS` skipping a
   missing index.
6. Add storage-engine smoke coverage for `RENAME INDEX IF EXISTS` renaming an
   existing supported secondary index.
7. Add storage-engine smoke coverage for representative
   `ALTER TABLE ... ADD INDEX IF NOT EXISTS` duplicate and missing-index forms.
8. Add storage-engine smoke coverage for representative
   `ALTER TABLE ... DROP INDEX IF EXISTS` missing and existing-index forms.
9. Assert warnings contain the skipped index names, forced-index reads work
   before/drop as appropriate, rows remain visible, close/reopen discovery is
   correct, and durable sidecar gates pass.
10. Run format, focused storage-smoke tests, harness reports, clang-tidy, and
   the `dev`, `embedded-dev`, and `storage-smoke-dev` gates.

## Acceptance Criteria

- Duplicate `CREATE INDEX IF NOT EXISTS existing_key ...` succeeds with a
  warning and leaves the existing index usable.
- Missing `CREATE INDEX IF NOT EXISTS created_key ...` succeeds and the new
  index supports forced-index reads before and after close/reopen until it is
  dropped.
- Missing `DROP INDEX IF EXISTS missing_key ...` succeeds with a warning and
  leaves the table unchanged.
- Existing `DROP INDEX IF EXISTS created_key ...` succeeds and the dropped
  index is unavailable after close/reopen.
- Duplicate `ALTER TABLE ... ADD INDEX IF NOT EXISTS category_key ...` succeeds
  with a warning and leaves the existing index usable.
- Missing `ALTER TABLE ... ADD INDEX IF NOT EXISTS alter_status_key ...`
  succeeds and the new index supports forced-index reads before and after
  close/reopen until it is dropped.
- Missing `ALTER TABLE ... DROP INDEX IF EXISTS missing_alter_key ...` succeeds
  with a warning and leaves the table unchanged.
- Existing `ALTER TABLE ... DROP INDEX IF EXISTS alter_status_key ...` succeeds
  and the dropped index is unavailable.
- Missing `RENAME INDEX IF EXISTS missing_status_key ...` succeeds with a
  warning and leaves the existing index usable.
- Existing `RENAME INDEX IF EXISTS status_key ...` succeeds, moves forced-index
  reads to the new name, and the renamed index can be dropped.
- Docs and compatibility matrices describe this as partial standalone routed
  index DDL support.

## Risks And Open Questions

- This slice intentionally tests warning message contents rather than exact
  MariaDB warning levels or ordering.
- Additional ALTER-table index spellings should get separate coverage if
  application schemas need those forms.
