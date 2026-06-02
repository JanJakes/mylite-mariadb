# Ownerless Row Format DDL Refresh

## Problem

Ownerless DDL coverage already proves many table-shape and metadata changes
refresh an already-open peer. InnoDB row-format changes are a separate
table-option path: MariaDB records `ROW_FORMAT` as a create option, InnoDB
requires a rebuild for explicit row-format changes, and the resulting metadata
is visible through InnoDB dictionary information schema tables. Ownerless mode
needs evidence that this rebuild-style table option crosses the shared
dictionary boundary and survives later reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ROW_FORMAT` table options and records
  `HA_CREATE_USED_ROW_FORMAT`.
- `mariadb/sql/sql_table.cc` marks an explicit ALTER TABLE row type as
  `HA_CREATE_USED_ROW_FORMAT` and carries it into ALTER TABLE execution.
- `mariadb/sql/handler.cc` treats a changed row type as not supported by the
  generic in-place handler API.
- `mariadb/storage/innobase/handler/handler0alter.cc` includes `ALTER_OPTIONS`
  in InnoDB rebuild operations and `alter_options_need_rebuild()` requires a
  rebuild when `HA_CREATE_USED_ROW_FORMAT` is specified.
- `mariadb/storage/innobase/handler/ha_innodb.cc` validates explicit InnoDB row
  formats and maps `ROW_FORMAT=COMPACT` and `ROW_FORMAT=DYNAMIC` to native
  InnoDB record formats.
- `mariadb/storage/innobase/handler/i_s.cc` exposes the native row format via
  `information_schema.INNODB_SYS_TABLES.ROW_FORMAT`.

## Scope And Non-Goals

- Add focused ownerless SQL coverage for
  `ALTER TABLE ... ROW_FORMAT=DYNAMIC` over an InnoDB table created with
  `ROW_FORMAT=COMPACT`.
- Verify an already-open ownerless peer observes the native row-format metadata
  before and after the DDL through `information_schema.INNODB_SYS_TABLES`.
- Verify existing rows remain readable and the already-open peer can insert
  after the row-format rebuild boundary.
- Verify final metadata and rows survive ownerless reopen, ordinary exclusive
  reopen, forced `.shm` rebuild, and exclusive reopen after rebuild.
- Do not claim every InnoDB row format, every `KEY_BLOCK_SIZE`, compressed-table
  option combination, or randomized DDL oracle coverage. A focused
  compressed-row-format rebuild is covered separately by
  `ownerless-compressed-row-format-ddl-refresh`.

## Design

Add a `row-format-ddl` selector to
`mylite_ownerless_cross_process_sql_test`:

1. A child ownerless process creates an InnoDB table with
   `ROW_FORMAT=COMPACT`, inserts rows, and signals the parent.
2. The already-open parent verifies `INNODB_SYS_TABLES.ROW_FORMAT = 'Compact'`
   and reads the inserted rows.
3. The child runs `ALTER TABLE ... ROW_FORMAT=DYNAMIC`.
4. The parent verifies `INNODB_SYS_TABLES.ROW_FORMAT = 'Dynamic'`, inserts a
   row, and checks final row aggregates.
5. Reopen helper assertions verify final row-format metadata and rows through
   ownerless/native reopen before and after forced `.shm` rebuild.

## Compatibility Impact

No public API behavior changes. The slice strengthens partial ownerless
DDL/dictionary compatibility for a rebuild-style InnoDB table option while
leaving broader DDL/file-lifecycle recovery and external oracle stress planned.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises existing native InnoDB table
files and ownerless concurrency files under the MyLite-owned database directory.

## Native Storage Impact

Native InnoDB storage remains MariaDB-managed. MyLite coordinates the ownerless
DDL boundary and verifies durable reopen behavior for the rebuilt native table.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `row-format-ddl` in `embedded-dev`.
- Build and run focused `row-format-ddl` in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- The already-open peer observes `ROW_FORMAT = 'Compact'` before the ALTER.
- The already-open peer observes `ROW_FORMAT = 'Dynamic'` after the ALTER.
- Existing rows remain readable and post-ALTER peer DML persists.
- Final metadata and rows survive ownerless/native reopen before and after
  forced `.shm` rebuild.
- Existing ownerless SQL and stress coverage remains green.

## Risks And Follow-Up

- This does not cover compressed or redundant row formats, `KEY_BLOCK_SIZE`,
- This does not cover redundant row format, every `KEY_BLOCK_SIZE`,
  table-option combinations, or randomized DDL oracles. Focused compressed
  row-format rebuild coverage is added by
  `ownerless-compressed-row-format-ddl-refresh`.
