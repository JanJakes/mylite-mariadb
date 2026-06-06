# Ownerless Pressure DDL Variant Policy

## Problem

The ownerless active-reader pressure policy throttles writes when retained
page-version WAL is at the configured soft limit while a live snapshot pin is
active. Existing pressure coverage includes representative DML, table DDL,
index DDL, schema create/drop, table-copy/replacement, view create/drop, and
trigger create/drop spellings. A remaining bounded gap is additional DDL
spellings that reuse established ownerless metadata paths but enter distinct
MariaDB parser or dictionary branches.

This slice extends the existing `active-reader-pressure-write-policy` selector
without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_ALTER_DB`,
  `SQLCOM_CREATE_TABLE`, `SQLCOM_DROP_TABLE`, `SQLCOM_CREATE_VIEW`,
  `SQLCOM_CREATE_TRIGGER`, and `SQLCOM_DROP_TRIGGER` with
  `CF_CHANGES_DATA`; `SQLCOM_CREATE_VIEW` also handles `ALTER VIEW`.
- `mariadb/sql/sql_db.cc` implements `ALTER DATABASE` default metadata updates
  through the native schema option path.
- `mariadb/sql/sql_table.cc` handles `CREATE TABLE IF NOT EXISTS` and
  `DROP TABLE IF EXISTS` branches.
- `mariadb/sql/sql_view.cc` persists `CREATE OR REPLACE VIEW` and
  `ALTER VIEW` metadata rewrites through native view definition files.
- `mariadb/sql/sql_trigger.cc` persists `CREATE OR REPLACE TRIGGER`,
  `CREATE TRIGGER IF NOT EXISTS`, and `DROP TRIGGER IF EXISTS` metadata through
  native trigger files.
- `packages/libmylite/src/database.cc` applies
  `enforce_ownerless_page_log_limit_policy()` before ownerless statement
  execution when `sql_statement_requires_write()` classifies the leading SQL
  token as write-like. The test must therefore cover user-visible spellings
  rather than relying only on MariaDB internal command names.

## Scope And Non-Goals

In scope:

- Extend `active-reader-pressure-write-policy` with:
  - `ALTER DATABASE ... DEFAULT CHARACTER SET/COLLATE`,
  - duplicate `CREATE TABLE IF NOT EXISTS`,
  - missing and real `DROP TABLE IF EXISTS`,
  - `CREATE OR REPLACE VIEW`,
  - `ALTER VIEW`,
  - `CREATE OR REPLACE TRIGGER`,
  - duplicate `CREATE TRIGGER IF NOT EXISTS`, and
  - missing and real `DROP TRIGGER IF EXISTS`.
- Verify each spelling returns `MYLITE_BUSY` while the pressure limit is
  reached and leaves schema, table, view, and trigger metadata unchanged.
- Verify the same statement families succeed after the reader pin releases and
  final state survives ownerless/native reopen before and after forced `.shm`
  rebuild.

Out of scope:

- Changing the pressure throttle classifier or checkpoint scheduler.
- `LOAD DATA` and file-import surfaces.
- SQL-level table-lock fault injection.
- Randomized pressure or RQG stress.

## Design

Reuse the retained-WAL setup from
`test_ownerless_active_reader_pressure_limit_blocks_write_classes()`:

1. Create baseline schema defaults, a table for idempotent DDL, a replacement
   view, a replaceable trigger, and an idempotent trigger before the reader pin
   starts.
2. Hold a repeatable-read snapshot in a peer process.
3. Commit one ownerless update so page-version WAL remains retained by the
   reader pin.
4. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
5. Assert each DDL variant returns `MYLITE_BUSY` and verify the baseline
   schema defaults, table shape, view columns, trigger action, and trigger
   presence remain unchanged.
6. Release the reader, execute the same DDL families successfully, and verify
   the final schema default, dropped idempotent table, altered view projection,
   replaced trigger effect, and dropped idempotent trigger through ownerless
   and native reopen, including forced shared-memory rebuild.

## Compatibility Impact

No public SQL behavior changes. The slice adds evidence that the existing
ownerless pressure throttle fails closed before additional MariaDB-compatible
metadata-changing DDL spellings can mutate native metadata while retained WAL is
over the configured limit.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises existing native schema
option files, table metadata/files, view definition files, trigger metadata
files, ownerless dictionary generation, page-version WAL retention,
checkpointing, and forced shared-memory rebuild.

## Native Storage Impact

No storage-format changes. Blocked statements must leave native files and
metadata untouched under pressure. After pressure clears, the same native
MariaDB paths remain usable and durable.

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
- Run the ownerless SQL CTest shard containing the selector in both presets.
- Run adjacent active-reader pressure stress coverage.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Each covered DDL variant returns `MYLITE_BUSY` with the pressure-limit
  diagnostic while a live snapshot pin retains WAL at the configured limit.
- Blocked variants leave schema defaults, table shape/presence, view
  projection, trigger bodies, and trigger presence unchanged.
- After the reader releases, the same statement families succeed and final
  state survives ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- The pressure classifier remains token-based and intentionally conservative for
  unknown leading keywords.
- This is deterministic SQL coverage, not randomized external pressure stress.
