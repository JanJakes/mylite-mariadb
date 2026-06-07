# Ownerless Pressure Replacement Copy Policy

## Problem

Ownerless active-reader pressure coverage already proves representative
table-copy and table-replacement DDL classes. A narrower remaining gap is the
combined replacement-copy spellings, `CREATE OR REPLACE TABLE ... LIKE` and
`CREATE OR REPLACE TABLE ... AS SELECT`: both drop or replace existing target
metadata while also copying source table shape or rows, so pressure throttling
must fail before MariaDB can leave a target in a mixed old/new state.

This slice extends the existing `active-reader-pressure-write-policy` selector
without changing production behavior.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` routes `CREATE TABLE` variants as
  metadata-changing SQL commands.
- `mariadb/sql/sql_table.cc` implements `CREATE TABLE ... LIKE`,
  `CREATE TABLE ... SELECT`, and `CREATE OR REPLACE TABLE` replacement paths.
- `packages/libmylite/src/database.cc` applies the
  `ownerless_page_log_limit_bytes` write throttle before ownerless statement
  lock acquisition, dictionary DDL begin, and MariaDB execution. The classifier
  is SQL-token based, so coverage must include the exact user-visible
  replacement-copy spellings.

## Design

Extend the retained-WAL setup in
`test_ownerless_active_reader_pressure_limit_blocks_write_classes()`:

1. Create source tables for replacement `LIKE` and CTAS paths, and create old
   replacement targets with old-only columns and indexes before the active
   reader starts.
2. Hold a repeatable-read snapshot in a peer process, commit one ownerless
   update, and reopen the writer with `ownerless_page_log_limit_bytes` set to
   the retained WAL size.
3. Assert both replacement-copy statements return `MYLITE_BUSY` with the
   pressure-limit diagnostic.
4. Verify the blocked statements leave the old target columns, old indexes, and
   rows intact, and do not expose copied columns or copied indexes.
5. Release the reader, execute both replacement-copy statements successfully,
   and verify final rows, copied metadata, and forced-index reads through
   ownerless reopen, native reopen, and forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- Pressure-limit coverage for `CREATE OR REPLACE TABLE ... LIKE`.
- Pressure-limit coverage for `CREATE OR REPLACE TABLE ... AS SELECT`.
- Blocked-state and final-state metadata checks for the replacement targets.

Out of scope:

- Changing the pressure policy or SQL classifier.
- Adding new crash hooks or recovery code.
- SQL-level table-lock fault injection.
- External randomized MariaDB/RQG pressure stress.

## Compatibility Impact

No public SQL or C API behavior changes. The slice adds evidence that the
existing ownerless pressure throttle returns `MYLITE_BUSY` before
metadata-changing replacement-copy SQL enters MariaDB while retained WAL is at
the configured soft cap.

## Directory And Lifecycle Impact

No directory layout changes. The test verifies the old native replacement
target remains intact under pressure and the final replacement-copy state
survives ownerless/native reopen and forced `.shm` rebuild after the reader
releases.

## Native Storage Impact

No storage-format changes. The covered SQL uses MariaDB native InnoDB table
creation, replacement, secondary-index, and CTAS population paths.

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
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Both replacement-copy statements return `MYLITE_BUSY` with the pressure-limit
  diagnostic while retained WAL is at the soft cap.
- The busy path preserves old replacement target columns, indexes, and rows.
- After reader release, both statement families succeed and the copied final
  state survives ownerless reopen, native reopen, and forced `.shm` rebuild.

## Risks And Follow-Up

- This deterministic selector does not replace broader randomized DDL pressure
  stress.
- Broader native redo/checkpoint reconciliation and DDL/file lifecycle recovery
  classes remain separate ownerless concurrency follow-up work.
