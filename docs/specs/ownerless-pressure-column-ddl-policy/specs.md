# Ownerless Pressure Column DDL Policy

## Problem

Ownerless active-reader pressure coverage proves the page-version WAL soft cap
blocks representative DML, table DDL, schema DDL, view DDL, and trigger DDL
while a live snapshot pin retains WAL. Column-shape DDL is a separate
high-value ALTER family because it mutates MariaDB table metadata and may use
copy, in-place, or instant native paths. The pressure selector did not yet
prove those supported column ALTER spellings are stopped before side effects
when retained WAL is already at the configured limit.

This slice extends the existing `active-reader-pressure-write-policy` selector
without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_ALTER_TABLE` with
  `CF_CHANGES_DATA`, so column ALTER belongs to MariaDB's write-like command
  family even when a specific option is metadata-only.
- `mariadb/sql/sql_yacc.yy` parses `CHANGE COLUMN`, `MODIFY COLUMN`,
  `DROP COLUMN`, `ALTER COLUMN ... SET DEFAULT`, `ALTER COLUMN ... DROP
  DEFAULT`, and `RENAME COLUMN` into `Alter_info` entries for the common ALTER
  path.
- `mariadb/sql/sql_table.cc:mysql_prepare_alter_table()` records column drop,
  change, default, and rename flags, and
  `mariadb/sql/sql_table.cc:mysql_alter_table()` executes the shared table
  ALTER path.
- `mariadb/storage/innobase/handler/handler0alter.cc` handles InnoDB column
  default, nullable/type, name, and type-change flags, preserving MariaDB's
  native storage semantics.
- `packages/libmylite/src/database.cc` runs
  `enforce_ownerless_page_log_limit_policy()` before ownerless statement
  locking, dictionary refresh, or MariaDB execution, using the existing SQL
  write classifier. The selector therefore needs to prove user-visible column
  ALTER statements return `MYLITE_BUSY` and leave metadata untouched.

## Scope And Non-Goals

In scope:

- Extend `active-reader-pressure-write-policy` with:
  - `ALTER TABLE ... MODIFY COLUMN`,
  - `ALTER TABLE ... CHANGE COLUMN`,
  - `ALTER TABLE ... DROP COLUMN`,
  - `ALTER TABLE ... RENAME COLUMN`,
  - `ALTER TABLE ... ALTER COLUMN ... SET DEFAULT`, and
  - `ALTER TABLE ... ALTER COLUMN ... DROP DEFAULT`.
- Verify each statement returns `MYLITE_BUSY` while a repeatable-read peer pin
  retains page-version WAL at the configured soft limit.
- Verify the blocked statements leave column names, types, defaults, and row
  data unchanged.
- Verify the same statements succeed after the reader releases, including final
  ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Production classifier changes.
- Unsupported ownerless table-lock or table-admin SQL surfaces.
- Randomized external MariaDB/RQG stress.
- Exhaustive combinations of generated columns, foreign keys, partitioning, and
  online DDL options under pressure.

## Design

Reuse the retained-WAL setup from
`test_ownerless_active_reader_pressure_limit_blocks_write_classes()`:

1. Create a dedicated InnoDB table with columns for type/default modification,
   rename, change, drop, set-default, and drop-default coverage.
2. Start a peer repeatable-read snapshot pin.
3. Commit one ownerless update so page-version WAL is retained by the pin.
4. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
5. Assert each column ALTER spelling returns `MYLITE_BUSY`.
6. Assert `INFORMATION_SCHEMA.COLUMNS` and row aggregates still expose the old
   column shape and defaults.
7. Release the reader pin, run the same column ALTER statements successfully,
   insert a row through the new defaults, and verify final column metadata/data
   through ownerless and native reopen, including forced shared-memory rebuild.

## Compatibility Impact

No public SQL behavior changes. The slice adds evidence that supported
MariaDB-compatible column ALTER statements are pressure-throttled at MyLite's
ownerless pre-execution gate before native metadata or row state can change
while retained WAL is over the configured limit.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises existing InnoDB table
metadata/files, ownerless dictionary generation, page-version WAL retention,
checkpointing, and forced shared-memory rebuild.

## Native Storage Impact

No storage-format changes. Blocked statements must not reach native InnoDB
mutation paths. After pressure clears, MariaDB's native ALTER TABLE machinery
remains responsible for durable column metadata and row/default behavior.

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
- Run the ownerless SQL shard or subset containing the selector.
- Run adjacent ownerless stress coverage for active-reader pressure.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Column ALTER modify, change, drop, rename, set-default, and drop-default
  statements return `MYLITE_BUSY` with the pressure-limit diagnostic while
  active-reader WAL pressure is at the configured limit.
- Blocked statements leave column metadata and row data unchanged.
- After the reader releases, the same statements succeed and final metadata/data
  survive ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- This is deterministic SQL coverage, not randomized pressure stress.
- Broader generated-column, foreign-key, partition, online-option, and external
  oracle combinations remain planned work.
