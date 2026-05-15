# CREATE TABLE IF NOT EXISTS

## Goal

Cover MariaDB-compatible `CREATE TABLE IF NOT EXISTS` behavior for routed
MyLite base tables. Creating a missing table should publish normal MyLite
catalog metadata. Creating an already existing routed table should succeed
with MariaDB warnings/notes and must not replace metadata, rows, indexes, or
requested-engine state.

## Non-Goals

- Do not cover every `CREATE TABLE ... LIKE` or `CREATE TABLE ... SELECT`
  `IF NOT EXISTS` combination in this slice.
- Do not cover temporary tables, views, partitions, foreign keys, triggers, or
  unsupported index classes.
- Do not change MyLite storage format, public API, or engine routing policy.
- Do not add broader transactional DDL beyond the existing statement-start
  checkpoint.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_yacc.yy:2522-2534` parses `CREATE ... TABLE
  opt_if_not_exists` and stores the option through
  `LEX::set_command_with_check()`.
- `mariadb/sql/sql_yacc.yy:5655-5664` maps `IF NOT EXISTS` to
  `DDL_options_st::OPT_IF_NOT_EXISTS`.
- `mariadb/sql/sql_table.cc:4635-4638` documents `create_table_impl()` returning
  `-1` when the table already existed and `IF NOT EXISTS` was used.
- `mariadb/sql/sql_table.cc:4760-4839` checks for existing non-temporary
  tables, performs `OR REPLACE` replacement when requested, and branches to the
  warning path instead of replacing when `options.if_not_exists()` is enabled.
- `mariadb/sql/sql_table.cc:13707-13770` routes regular non-`LIKE`,
  non-`SELECT` creates through `mysql_create_table()` and sends OK when
  creation succeeds.
- `packages/libmylite/src/database.cc:is_storage_outer_checkpoint_sql()` wraps
  `CREATE` statements in a MyLite statement checkpoint for file-backed
  databases.

## Compatibility Impact

This slice covers partial MariaDB compatibility for ordinary routed
`CREATE TABLE IF NOT EXISTS` statements:

- missing targets create catalog-backed routed MyLite tables;
- existing targets are warning-producing no-ops;
- existing target metadata, rows, supported indexes, and requested-engine
  metadata remain unchanged;
- close/reopen discovery sees the committed catalog state.

The behavior remains partial because `LIKE`, CTAS, temporary tables, views,
partitions, foreign keys, and every mixed DDL shape still require separate
matrices.

## Design

No production code change is expected. MariaDB owns the existence check and
warning semantics. MyLite receives handler create calls only for targets that
MariaDB decides to create.

The MyLite-specific risk is accidental publication on the skip path. The test
must prove that a skipped existing target keeps its original requested engine,
rows, and supported index behavior even when the skipped statement names a
different table definition and engine.

## Affected Subsystems

- MariaDB parser and table-create execution determine the compatibility
  semantics.
- MyLite storage catalog create hooks publish missing-table metadata.
- MyLite warning capture exposes MariaDB diagnostics after successful direct
  execution.
- MyLite statement checkpoints protect file-backed `CREATE` publication paths.

## DDL Metadata Routing Impact

Missing `CREATE TABLE IF NOT EXISTS` targets publish the same catalog records
as ordinary routed `CREATE TABLE` statements. Existing targets do not publish
new records and do not alter the existing record.

## Single-File And Lifecycle Impact

Successful creates and skipped existing-target statements must not create
persistent `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, `aria_log.*`,
binlog, relay-log, or plugin-owned table sidecars. After final close, the
MyLite runtime directory must be empty.

## Public API, File Format, Build, And Dependency Impact

There are no public API, file-format, build-profile, binary-size, license, or
dependency changes.

## Test And Verification Plan

1. Add storage-engine smoke coverage for creating a missing routed
   `CREATE TABLE IF NOT EXISTS` target.
2. Add storage-engine smoke coverage for skipping an existing routed target
   when the skipped statement specifies a different definition and requested
   engine.
3. Assert the skipped target's warning rows include the existing table name.
4. Assert catalog count, requested/effective engine metadata, row visibility,
   supported index behavior, close/reopen discovery, and durable sidecar gates.
5. Run format, focused storage-smoke tests, harness reports, clang-tidy, and
   the `dev`, `embedded-dev`, and `storage-smoke-dev` gates.

## Acceptance Criteria

- `CREATE TABLE IF NOT EXISTS missing ... ENGINE=Aria` creates a routed MyLite
  catalog record with requested engine `Aria`.
- `CREATE TABLE IF NOT EXISTS existing ... ENGINE=MyISAM` succeeds with a
  warning and preserves the existing table's original requested engine,
  definition, rows, and supported index reads.
- Reopened file-backed sessions discover the same committed state without
  runtime schema directories or forbidden durable sidecars.
- Docs and compatibility matrices describe the covered `IF NOT EXISTS`
  behavior as partial ordinary routed base-table support.

## Risks And Open Questions

- MariaDB warning wording is not part of MyLite's stable API; tests should
  assert that the relevant table name appears rather than requiring an exact
  message.
- `CREATE TABLE IF NOT EXISTS ... LIKE` and CTAS should get separate coverage
  if application schemas need those forms.
