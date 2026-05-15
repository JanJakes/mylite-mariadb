# Column ALTER IF EXISTS

## Goal

Cover MariaDB-compatible column-level existence options for routed copy
`ALTER TABLE` DDL. `ADD COLUMN IF NOT EXISTS` should skip existing columns with
warnings and add missing columns through MyLite's copy-rebuild path.
`DROP COLUMN IF EXISTS` should skip missing columns with warnings and drop
existing columns without losing rows, indexes, or catalog metadata.

## Non-Goals

- Do not cover every `CHANGE COLUMN IF EXISTS` or `MODIFY COLUMN IF EXISTS`
  shape in this slice.
- Do not add online/in-place column DDL; MyLite still rejects online and
  in-place ALTER requests.
- Do not cover generated columns, CHECK constraints, foreign keys, partitions,
  or trigger interactions beyond the existing dedicated slices.
- Do not change storage format, public API, or row payload layout.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_yacc.yy:7970-7981` parses `ADD COLUMN
  opt_if_not_exists_table_element` and marks the column create entry.
- `mariadb/sql/sql_yacc.yy:8014-8018` parses `CHANGE COLUMN
  opt_if_exists_table_element`; `MODIFY COLUMN IF EXISTS` is handled by the
  same alter create-list machinery.
- `mariadb/sql/sql_yacc.yy:8030-8039` parses `DROP COLUMN
  opt_if_exists_table_element` into an `Alter_drop::COLUMN`.
- `mariadb/sql/sql_table.cc:6270-6313` removes duplicate
  `ADD COLUMN IF NOT EXISTS` entries and pushes note-level diagnostics.
- `mariadb/sql/sql_table.cc:6316-6351` removes missing
  `MODIFY/CHANGE COLUMN IF EXISTS` entries and pushes note-level diagnostics.
- `mariadb/sql/sql_table.cc:6386-6512` removes missing
  `DROP COLUMN IF EXISTS` entries and pushes note-level diagnostics.

## Compatibility Impact

This slice covers partial MariaDB compatibility for routed copy
`ALTER TABLE` column existence options:

- duplicate column adds are warning-producing no-ops;
- missing column adds rebuild the table definition and rows through MyLite;
- missing column drops are warning-producing no-ops;
- existing column drops rebuild the table definition and rows without the
  dropped column;
- close/reopen discovery sees the committed column state.

The behavior remains partial because change/modify variants, generated-column
edge cases, CHECK/foreign-key interactions, online/in-place algorithms, and
broader rollback matrices remain separate work.

## Design

No production code change is expected. MariaDB filters skipped existence-option
items before executing the ALTER. MyLite handles the remaining supported column
adds and drops through its existing copy-rebuild path.

The MyLite-specific risk is rebuild publication. The test must prove that
skipped duplicate adds and skipped missing drops leave the table unchanged,
while actual adds and drops preserve existing rows and supported indexes before
and after close/reopen.

## Affected Subsystems

- MariaDB parser and ALTER preparation determine existence-option semantics.
- MyLite copy-rebuild DDL publishes the rebuilt table definition and rows.
- MyLite warning capture exposes MariaDB diagnostics after successful direct
  execution.
- MyLite statement checkpoints protect file-backed ALTER publication paths.

## DDL Metadata Routing Impact

Supported column adds and drops append a rebuilt table definition inside the
MyLite catalog. Skipped existence-option operations do not publish a new
definition.

## Single-File And Lifecycle Impact

Successful column alters and skipped existence-option statements must not
create persistent `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`,
`aria_log.*`, binlog, relay-log, or plugin-owned table sidecars. After final
close, the MyLite runtime directory must be empty.

## Public API, File Format, Build, And Dependency Impact

There are no public API, file-format, build-profile, binary-size, license, or
dependency changes.

## Test And Verification Plan

1. Add storage-engine smoke coverage for `ADD COLUMN IF NOT EXISTS` skipping an
   existing column.
2. Add storage-engine smoke coverage for `ADD COLUMN IF NOT EXISTS` creating a
   missing ordinary column with a default.
3. Add storage-engine smoke coverage for `DROP COLUMN IF EXISTS` skipping a
   missing column.
4. Add storage-engine smoke coverage for `DROP COLUMN IF EXISTS` dropping the
   added column.
5. Assert warnings contain the skipped column names, row values and supported
   index reads survive each operation, dropped columns are unavailable after
   close/reopen, and durable sidecar gates pass.
6. Run format, focused storage-smoke tests, harness reports, clang-tidy, and
   the `dev`, `embedded-dev`, and `storage-smoke-dev` gates.

## Acceptance Criteria

- Duplicate `ADD COLUMN IF NOT EXISTS title ...` succeeds with a warning and
  leaves existing rows and indexes unchanged.
- Missing `ADD COLUMN IF NOT EXISTS subtitle ... DEFAULT ...` succeeds and
  default values are visible before and after close/reopen until the column is
  dropped.
- Missing `DROP COLUMN IF EXISTS missing_subtitle` succeeds with a warning and
  leaves the table unchanged.
- Existing `DROP COLUMN IF EXISTS subtitle` succeeds and the dropped column is
  unavailable after close/reopen.
- Docs and compatibility matrices describe this as partial routed copy ALTER
  column existence-option support.

## Risks And Open Questions

- This slice intentionally asserts warning message contents rather than exact
  MariaDB warning levels or ordering.
- `CHANGE/MODIFY COLUMN IF EXISTS` should get separate coverage if application
  migrations need those spellings.
