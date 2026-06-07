# Ownerless CTAS Insert Pressure Policy

## Summary

Ownerless active-reader pressure policy must throttle post-create
`INSERT ... SELECT` into an existing CTAS destination, matching the existing
CTAS post-create `UPDATE` and `DELETE` pressure coverage.

## Problem

The `ownerless-ctas-pressure-policy` slice proved that `UPDATE` and `DELETE`
against a CTAS-created table return `MYLITE_BUSY` while a live snapshot pin
retains page-version WAL at the configured soft limit. The later CTAS
post-create DML slice broadened the success path to include `INSERT ... SELECT`,
leaving pressure coverage asymmetric for the same CTAS destination.

Without this focused proof, the active-reader pressure matrix could claim CTAS
post-create DML coverage while only proving insert-select on the non-pressure
success path.

## Source References

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_INSERT_SELECT` with
  `CF_CHANGES_DATA` and `CF_INSERTS_DATA`, so MariaDB treats this command as a
  data-changing statement.
- `mariadb/sql/sql_lex.h` includes `SQLCOM_INSERT_SELECT` in the DML commands
  where check-option handling matters.
- MyLite's `packages/libmylite/src/database.cc` pressure preflight is token
  based through `sql_statement_requires_write()`, so representative SQL
  spellings need direct coverage.

## Scope And Non-Goals

In scope:

- Extend the existing `active-reader-pressure-write-policy` selector.
- Require `INSERT INTO <ctas> SELECT ...` to return `MYLITE_BUSY` while the
  ownerless page-version WAL is at the active-reader soft cap.
- Verify the blocked insert-select leaves the CTAS table row count and
  aggregate unchanged.
- Run the same insert-select after the snapshot pin releases and verify final
  state through ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- Exhaustive CTAS post-create DML isolation or crash matrices.
- New production pressure-policy behavior.
- Full randomized external pressure stress.

## Design

Reuse the existing retained-WAL pressure setup:

1. Create `app.ownerless_pressure_existing_ctas` from the pressure-policy base
   table before a stale reader starts.
2. Hold a repeatable-read ownerless snapshot pin and commit an update that
   leaves page-version WAL retained.
3. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
4. Require CTAS-destination `UPDATE`, `DELETE`, and now `INSERT ... SELECT` to
   return `MYLITE_BUSY`.
5. Verify the blocked CTAS DML leaves the CTAS table at 2 rows with
   `SUM(value)=30`.
6. Release the reader, execute the same CTAS insert-select after the successful
   update/delete sequence, and verify the final CTAS table has 2 rows with
   `SUM(value)=48`.

## Compatibility Impact

No SQL behavior changes. This slice broadens evidence for the existing opt-in
ownerless active-reader pressure throttle. CTAS pressure coverage remains
partial because exhaustive CTAS DML/crash matrices and randomized external
pressure stress remain planned.

## Database Directory And Lifecycle Impact

No directory layout changes. The focused selector already verifies the final
CTAS destination through live ownerless state, ownerless reopen, native
exclusive reopen, and the same two reopen paths after forced shared-memory
rebuild.

## Native Storage Impact

No storage format changes. The blocked insert-select must not enter native
InnoDB mutation paths while pressure is active; after pressure clears, native
InnoDB handles the ordinary CTAS table insert-select path.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. The slice adds test and documentation
coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `active-reader-pressure-write-policy` in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run adjacent active-reader pressure selectors.
- Run the relevant ownerless SQL shard.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Existing CTAS table starts with 2 rows and `SUM(value)=30`.
- While retained WAL is at the soft cap, CTAS `UPDATE`, `DELETE`, and
  `INSERT ... SELECT` return `MYLITE_BUSY`.
- The blocked CTAS DML leaves the table at 2 rows and `SUM(value)=30`.
- After the reader releases, the CTAS DML succeeds and leaves 2 rows with
  `SUM(value)=48`.
- Ownerless/native reopen before and after forced `.shm` rebuild observe that
  final CTAS state.

## Risks And Follow-Up

- This remains deterministic focused coverage, not a replacement for full
  randomized external MariaDB/RQG pressure stress.
- Broader CTAS crash matrices remain planned.
