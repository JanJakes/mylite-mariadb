# Table DDL IF EXISTS

## Goal

Cover MariaDB-compatible `IF EXISTS` skip semantics for routed MyLite table DDL.
`DROP TABLE IF EXISTS` and `RENAME TABLE IF EXISTS` should succeed when a listed
source table is missing, emit MariaDB warnings/notes through `libmylite`, and
apply the requested changes for existing routed base tables without creating
durable MariaDB sidecars.

## Non-Goals

- Do not add broader transactional DDL beyond the existing statement-start
  checkpoint.
- Do not add view, trigger, partition, foreign-key, or temporary-table `IF
  EXISTS` matrices in this slice.
- Do not implement `IF EXISTS` for unsupported object classes or server
  surfaces.
- Do not change storage file format, catalog record layout, or public API.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_yacc.yy:8712-8720` parses `RENAME table_or_tables
  opt_if_exists table_to_table_list`, so MariaDB's accepted syntax is
  `RENAME TABLE IF EXISTS old_name TO new_name`.
- `mariadb/sql/sql_yacc.yy:13421-13430` parses `DROP opt_temporary
  table_or_tables opt_if_exists table_list`, so MariaDB's accepted syntax is
  `DROP TABLE IF EXISTS name`.
- `mariadb/sql/sql_rename.cc:276-307` reports a missing source table as a note
  and returns a negative skip result when `if_exists` is enabled.
- `mariadb/sql/sql_rename.cc:505-562` skips negative `check_rename()` results
  and continues renaming the remaining pairs, while ordinary errors still
  revert prior renames.
- `mariadb/sql/sql_table.cc:1145-1248` passes the parsed `if_exists` flag into
  the drop-table implementation after acquiring normal table-name locks.
- `mariadb/sql/sql_table.cc:1830-1868` accumulates missing-table diagnostics
  and suppresses missing-table errors when `if_exists` is enabled.
- `packages/libmylite/src/database.cc:is_storage_outer_checkpoint_sql()` wraps
  `DROP` and `RENAME` statements in a MyLite statement checkpoint for
  file-backed databases.

## Compatibility Impact

This slice covers partial MariaDB compatibility for routed base-table
`IF EXISTS` table DDL:

- missing `DROP TABLE IF EXISTS` targets are warning-producing no-ops;
- existing `DROP TABLE IF EXISTS` targets are removed from MyLite catalog
  metadata;
- missing `RENAME TABLE IF EXISTS` source pairs are skipped with warnings;
- existing `RENAME TABLE IF EXISTS` source pairs are renamed through the normal
  MyLite catalog path;
- close/reopen discovery sees only the committed catalog state.

The behavior remains partial because this slice does not cover views,
temporary tables, triggers, foreign keys, partitioned tables, or every mixed
multi-table ordering.

## Design

No production code change is expected. MariaDB owns the parser and skip
semantics, while MyLite already owns the catalog mutation hooks for routed
`DROP TABLE` and `RENAME TABLE`.

The MyLite-specific risk is publication order: a successful statement that
skips some missing objects and mutates others must commit exactly once through
the statement checkpoint. A failing statement without `IF EXISTS` must still
roll back through the existing failed table-DDL coverage.

## Affected Subsystems

- MariaDB parser and SQL DDL execution determine compatibility semantics.
- MyLite `libmylite` warning capture exposes MariaDB notes after successful
  direct execution.
- MyLite storage catalog drop and rename hooks publish the durable metadata
  changes.
- MyLite statement checkpoints protect the existing rollback behavior for
  non-`IF EXISTS` failures.

## DDL Metadata Routing Impact

`IF EXISTS` does not introduce a new storage operation. It changes the SQL-layer
decision about whether a missing table is an error. Existing routed tables still
flow through the same MyLite catalog drop and rename paths, and skipped missing
tables leave no catalog record behind.

## Single-File And Lifecycle Impact

The only durable application state remains the primary `.mylite` file. The
slice must not create `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`,
`aria_log.*`, binlog, relay-log, or plugin-owned table sidecars. After final
close, the MyLite runtime directory must be empty.

## Public API, File Format, Build, And Dependency Impact

There are no public API, file-format, build-profile, binary-size, license, or
dependency changes. The existing warning API should expose MariaDB diagnostics
for successful `IF EXISTS` statements.

## Test And Verification Plan

1. Add storage-engine smoke coverage for `DROP TABLE IF EXISTS` over an
   existing routed table and a missing table in the same statement.
2. Add storage-engine smoke coverage for a missing-only `DROP TABLE IF EXISTS`
   no-op.
3. Add storage-engine smoke coverage for `RENAME TABLE IF EXISTS` with one
   missing source pair and one existing routed source pair.
4. Assert MyLite warning rows include the skipped missing table names.
5. Assert catalog metadata, row visibility, indexed lookups, close/reopen
   discovery, and durable sidecar gates after the statements.
6. Run format, focused storage-smoke tests, harness reports, clang-tidy, and
   the `dev`, `embedded-dev`, and `storage-smoke-dev` gates.

## Acceptance Criteria

- `DROP TABLE IF EXISTS existing, missing` succeeds, drops `existing`, leaves
  no `missing` catalog record, and emits a missing-table warning/note.
- `DROP TABLE IF EXISTS missing_only` succeeds without changing existing
  catalog records and emits a missing-table warning/note.
- `RENAME TABLE IF EXISTS missing TO skipped, existing TO renamed` succeeds,
  skips the missing pair, renames the existing routed table, preserves rows and
  supported index reads, and emits a missing-table warning/note.
- Reopened file-backed sessions discover the committed catalog state without
  runtime schema directories or forbidden durable sidecars.
- Docs and compatibility matrices describe the covered `IF EXISTS` behavior as
  partial routed base-table support.

## Risks And Open Questions

- MariaDB's warning level for skipped objects is intentionally not normalized
  by MyLite; tests should assert the missing-object content rather than a
  brittle exact warning order.
- Temporary-table, view, trigger, partition, and foreign-key interactions still
  need broader compatibility slices.
