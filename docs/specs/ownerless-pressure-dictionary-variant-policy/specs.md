# Ownerless Pressure Dictionary Variant Policy

## Problem

The ownerless active-reader pressure policy throttles writes when a live
snapshot pin retains page-version WAL at the configured soft limit. Existing
coverage proves direct/prepared DML, representative table DDL, index DDL,
rename, and truncate spellings. A remaining bounded gap is dictionary-heavy
write forms that use schema, table-copy, replacement, view, and trigger command
paths but must still be blocked before MariaDB mutates native metadata while
the pressure limit is reached.

This slice extends the existing `active-reader-pressure-write-policy` selector
without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks schema, table, view, trigger, insert,
  replace, rename, drop, and truncate command classes as data or metadata
  changing SQL commands.
- `mariadb/sql/sql_table.cc` implements `CREATE TABLE ... LIKE`,
  `CREATE TABLE ... SELECT`, `CREATE OR REPLACE TABLE`, `DROP DATABASE`, and
  table rename/truncate file-lifecycle paths.
- `mariadb/sql/sql_view.cc` persists view metadata through native view
  definition files.
- `mariadb/sql/sql_trigger.cc` persists table trigger metadata through `.TRG`
  and `.TRN` files.
- `packages/libmylite/src/database.cc` enforces
  `ownerless_page_log_limit_bytes` before ownerless statement locks,
  dictionary DDL begin, and MariaDB execution. The policy uses
  `sql_statement_requires_write()` over leading SQL tokens, so tests must cover
  user-visible spellings rather than only internal MariaDB command names.

## Design

Extend the retained-WAL setup from
`test_ownerless_active_reader_pressure_limit_blocks_write_classes()`:

1. Create baseline schema, table-replacement, view, and trigger objects before
   the active reader pin starts.
2. Hold a repeatable-read snapshot in a peer process and commit one ownerless
   update so page-version WAL remains retained by that pin.
3. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
4. Assert `MYLITE_BUSY` with the pressure-limit diagnostic for:
   - `REPLACE ... SELECT`,
   - `CREATE DATABASE`,
   - `DROP DATABASE`,
   - `CREATE TABLE ... LIKE`,
   - CTAS,
   - `CREATE OR REPLACE TABLE`,
   - `CREATE VIEW`,
   - `DROP VIEW`,
   - `CREATE TRIGGER`, and
   - `DROP TRIGGER`.
5. Verify every blocked variant leaves rows, schemas, tables, views, triggers,
   and replacement-table columns unchanged.
6. Release the reader, execute the same variants successfully, and verify final
   state through ownerless reopen, ordinary native reopen, and forced `.shm`
   rebuild.

## Scope And Non-Goals

In scope:

- Pressure-limit coverage for dictionary-heavy SQL write spellings.
- Final-state checks for schema, table-copy, replacement, view, and trigger
  metadata.
- Documentation and compatibility matrix updates.

Out of scope:

- Changing the pressure policy, page-version WAL format, or checkpoint
  scheduler.
- `LOAD DATA` or file-import surfaces, which remain governed by server/file
  policy coverage.
- Randomized pressure/RQG stress.
- SQL-level table-lock fault injection.

## Compatibility Impact

No public SQL or C API behavior changes. The selector adds evidence that the
existing ownerless pressure throttle fails closed before metadata-changing SQL
can enter MariaDB while a retained-WAL snapshot pin is over the configured
limit.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises existing native schema
directories, table files, view metadata files, trigger metadata files,
ownerless dictionary generation, page-version WAL retention, checkpointing, and
forced shared-memory rebuild.

## Native Storage Impact

No storage-format changes. Blocked statements must leave native files and
metadata untouched while pressure is active. After pressure clears, the same
statements use the existing native MariaDB paths and final state survives
ownerless/native reopen.

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
- Run adjacent active-reader pressure selectors.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Each covered dictionary-heavy write spelling returns `MYLITE_BUSY` with the
  pressure-limit diagnostic while the reader pin is active.
- Blocked statements do not create, drop, replace, or rewrite schemas, tables,
  views, triggers, or rows.
- Once the reader releases, the same statement family succeeds and the final
  state survives ownerless reopen, ordinary native reopen, and forced `.shm`
  rebuild.

## Risks And Follow-Up

- The token-based pressure classifier remains conservative for unknown leading
  keywords.
- This is deterministic SQL coverage, not randomized pressure stress. External
  MariaDB/RQG pressure oracles remain planned.
