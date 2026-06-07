# Ownerless Pressure Storage DDL Policy

## Problem

Ownerless active-reader pressure throttling now covers representative DML,
table/schema/view/trigger DDL, CTAS post-create DML, column ALTER variants, and
CHECK/FOREIGN KEY add/drop. Storage-oriented table ALTERs remain a useful
bounded pressure-policy gap because they exercise native rebuild and metadata
paths that can touch file lifecycle, table definitions, and InnoDB row-format
state.

This slice extends the existing `active-reader-pressure-write-policy` selector
without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_ALTER_TABLE` with
  `CF_CHANGES_DATA`.
- `mariadb/sql/sql_yacc.yy` parses `ALTER TABLE ... CONVERT TO CHARACTER SET`,
  `ALTER TABLE ... FORCE`, and table option changes such as `ROW_FORMAT` into
  the common ALTER TABLE command.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` executes the shared ALTER
  path and prepares table definition changes, rebuild decisions, and table
  options.
- `mariadb/storage/innobase/handler/handler0alter.cc` applies InnoDB online,
  copy, and rebuild decisions for ALTER TABLE, including row-format-sensitive
  metadata.
- `packages/libmylite/src/database.cc` runs
  `enforce_ownerless_page_log_limit_policy()` before ownerless statement
  locking, dictionary refresh, or MariaDB execution. The test must prove these
  storage/rebuild ALTERs return `MYLITE_BUSY` and leave metadata/data
  unchanged at the pressure boundary.

## Scope And Non-Goals

In scope:

- Extend `active-reader-pressure-write-policy` with:
  - `ALTER TABLE ... CONVERT TO CHARACTER SET ... COLLATE ...`,
  - `ALTER TABLE ... FORCE`, and
  - `ALTER TABLE ... ROW_FORMAT=DYNAMIC`.
- Verify each statement returns `MYLITE_BUSY` while a repeatable-read peer pin
  retains page-version WAL at the configured soft limit.
- Verify blocked statements leave column collation, row-format metadata, and
  row payloads unchanged.
- Verify the same statements succeed after the reader releases and final state
  survives ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Exhaustive row-format, compression, encryption, tablespace, or partition
  matrices.
- Production pressure classifier changes.
- Crash-boundary DDL file-lifecycle metadata design.
- Randomized external MariaDB/RQG stress.

## Design

Reuse the retained-WAL setup from
`test_ownerless_active_reader_pressure_limit_blocks_write_classes()`:

1. Create dedicated baseline InnoDB tables for charset conversion, forced
   rebuild, and row-format conversion.
2. Hold a repeatable-read snapshot in a peer ownerless process.
3. Commit one ownerless update so page-version WAL remains retained.
4. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
5. Assert the storage/rebuild ALTER statements return `MYLITE_BUSY`.
6. Assert blocked state still shows the original `latin1` collation, `Compact`
   row format, and unchanged row/payload aggregates.
7. Release the reader, run the same ALTER statements successfully, and verify
   `utf8mb4` collation, `Dynamic` row format, and post-rebuild DML.
8. Verify final metadata and aggregates through ownerless/native reopen before
   and after forced shared-memory rebuild.

## Compatibility Impact

No SQL behavior changes. The slice adds evidence that supported storage and
rebuild ALTER statements are pressure-throttled before native metadata, row, or
file state can change while retained WAL is over the configured ownerless
limit.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises existing InnoDB table
metadata/files, ownerless dictionary generation, page-version WAL retention,
checkpointing, and forced shared-memory rebuild.

## Native Storage Impact

No storage-format changes. Blocked statements must not reach native InnoDB
rebuild or row-format mutation paths. After pressure clears, MariaDB's native
ALTER TABLE machinery remains responsible for durable metadata and rows.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds test and documentation
coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `active-reader-pressure-write-policy` selector in
  `embedded-dev`.
- Build and run the same focused selector in `ownerless-test-hooks`.
- Run the ownerless SQL shard containing the selector in both presets.
- Run adjacent active-reader pressure stress and trace checks.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Storage/rebuild ALTER statements return `MYLITE_BUSY` with the pressure-limit
  diagnostic while active-reader WAL pressure is at the configured limit.
- Blocked statements leave charset/collation, row-format metadata, and row
  aggregates unchanged.
- After the reader releases, the same statements succeed and final
  metadata/data survive ownerless/native reopen before and after forced `.shm`
  rebuild.

## Risks And Follow-Up

- This is representative deterministic SQL coverage, not an exhaustive storage
  ALTER matrix.
- Broader compressed/encrypted/tablespace/partition variants and randomized
  external oracle pressure combinations remain planned.
