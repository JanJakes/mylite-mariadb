# Ownerless Pressure Write Variant Policy

## Problem

The ownerless pressure policy already blocks representative direct
`INSERT`/`UPDATE`/`DELETE` and table `CREATE`/`ALTER`/`DROP` statements while a
repeatable-read snapshot pin retains page-version WAL at the configured soft
limit. The remaining coverage gap is variant write spellings that enter
different MariaDB SQL command paths or table lifecycle paths but should still be
throttled before execution under the same pressure condition.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_CREATE_INDEX`,
  `SQLCOM_DROP_INDEX`, `SQLCOM_RENAME_TABLE`, `SQLCOM_TRUNCATE`,
  `SQLCOM_INSERT_SELECT`, `SQLCOM_REPLACE`, and `SQLCOM_REPLACE_SELECT` with
  `CF_CHANGES_DATA`.
- `mariadb/sql/sql_lex.h` distinguishes multi-table `UPDATE`/`DELETE`,
  `REPLACE`, `REPLACE ... SELECT`, `INSERT ... SELECT`, and `LOAD` as
  updating statements.
- MyLite's `sql_statement_requires_write()` is intentionally token based, so
  the pressure preflight must be validated against user-visible SQL spellings,
  not only the simplest statement form for each leading keyword.

## Scope And Non-Goals

In scope:

- Extend the focused `active-reader-pressure-write-policy` selector with
  `REPLACE`, `INSERT ... SELECT`, multi-table `UPDATE`, multi-table `DELETE`,
  `CREATE INDEX`, `DROP INDEX`, `RENAME TABLE`, and `TRUNCATE TABLE`.
- Verify each spelling returns `MYLITE_BUSY` before mutation while pressure is
  active.
- Verify the same handle can execute those spellings after the active snapshot
  pin releases.
- Verify final state through ownerless and native exclusive reopen before and
  after forced `.shm` rebuild.

Out of scope:

- `LOAD DATA` file import, which remains governed by MyLite's server/file
  surface policies.
- Background checkpoint scheduling.
- External randomized pressure stress.

## Design

Reuse the existing retained-WAL pressure setup:

1. Create durable baseline tables, including an existing secondary index, a
   table to rename, a table to truncate, and a join table for multi-table DML.
2. Hold a repeatable-read snapshot pin in a peer process.
3. Commit one ownerless update so page-version WAL remains retained by the pin.
4. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
5. Assert the variant write statements all fail with the pressure-limit
   diagnostic and leave rows, indexes, table names, and truncate target state
   unchanged.
6. Release the reader and execute the same variant statements successfully.
7. Reopen through ownerless and ordinary exclusive modes, before and after
   forced `.shm` rebuild, and verify the final row, index, rename, truncate,
   and drop state.

## Compatibility Impact

SQL behavior is unchanged except for stronger evidence that the existing
ownerless pressure throttle applies consistently across MariaDB-compatible
write spellings. The policy remains a pre-execution soft cap.

## Database Directory And Lifecycle Impact

No directory layout changes. The test covers additional metadata and file
lifecycle transitions through the existing ownerless dictionary generation,
page-version WAL, checkpoint, and forced shared-memory rebuild paths.

## Native Storage Impact

No storage format changes. Blocked statements must not enter native InnoDB
mutation paths while pressure is active; the same native paths remain usable
after pressure clears.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds test and documentation
coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `active-reader-pressure-write-policy` selector in
  `embedded-dev`.
- Run the focused selector in `ownerless-test-hooks`.
- Run adjacent pressure selectors in embedded and hook builds.
- Run full embedded and hook ownerless cross-process SQL CTest coverage.
- Run the active-reader and expanding-page pressure stress selectors in
  `ownerless-stress`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Each covered variant returns `MYLITE_BUSY` with the pressure-limit diagnostic
  while a live snapshot pin retains WAL at the configured limit.
- The blocked variants leave row values, row counts, secondary-index metadata,
  table names, truncate target contents, and drop target presence unchanged.
- After the reader releases, the same handle executes the variants and the
  final state survives ownerless/native reopen before and after forced `.shm`
  rebuild.

## Risks And Follow-Up

- The token-based classifier remains intentionally conservative for unknown
  leading keywords.
- Broader randomized pressure stress and background checkpoint scheduling remain
  planned.
