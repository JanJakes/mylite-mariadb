# Ownerless CTAS Pressure Policy

## Summary

Ownerless active-reader pressure policy must apply to post-create DML on an
existing CTAS destination, not only to ordinary base tables or CTAS creation.
This slice adds focused coverage proving `UPDATE` and `DELETE` against a
CTAS-created table are blocked before execution while a stale snapshot pin
keeps page-version WAL at the configured soft limit. The later
`ownerless-ctas-insert-pressure-policy` follow-up adds matching
`INSERT ... SELECT` coverage for the same CTAS destination.

## Problem

The active-reader pressure policy already covers direct/prepared writes,
representative DML/DDL classes, and CTAS creation under pressure. After the
CTAS post-create DML refresh fix, the remaining bounded pressure question is
whether existing CTAS destinations are treated like ordinary writable tables
when retained WAL reaches the soft cap.

Without this coverage, MyLite could accidentally claim broad CTAS pressure
behavior while only proving the create-table-copy path, not later DML against
the CTAS result.

## Design

Extend `active-reader-pressure-write-policy`:

1. Create `app.ownerless_pressure_existing_ctas` from the pressure-policy base
   table before the stale reader starts.
2. Hold a repeatable-read ownerless snapshot pin and commit an update that
   leaves page-version WAL retained.
3. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
4. Require CTAS-destination `UPDATE`, `DELETE`, and follow-up
   `INSERT ... SELECT` coverage to return `MYLITE_BUSY`.
5. Verify the CTAS table count and aggregate remain unchanged under pressure.
6. Release the reader, rerun the same CTAS DML successfully, and verify
   ownerless/native reopen before and after forced `.shm` rebuild.

## Compatibility Impact

No SQL behavior changes. The slice broadens active-reader pressure evidence for
CTAS-created tables while keeping the pressure policy opt-in and partial.
Exhaustive CTAS DML/crash matrices and randomized external pressure stress
remain planned.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `active-reader-pressure-write-policy` in `embedded-dev`.
- Run adjacent active-reader pressure selectors in embedded and hook presets.
- Run `ownerless-stress`, `format-check`, `git diff --check`, cached diff
  checks, and temp/process cleanup checks.

## Acceptance Criteria

- Existing CTAS table starts with 2 rows and `SUM(value)=30`.
- While retained WAL is at the soft cap, CTAS `UPDATE`, `DELETE`, and the
  follow-up `INSERT ... SELECT` coverage return `MYLITE_BUSY`.
- The blocked CTAS DML leaves the table at 2 rows and `SUM(value)=30`.
- After the reader releases, the CTAS DML succeeds and the follow-up
  insert-select coverage leaves 2 rows with `SUM(value)=48`.
- Ownerless/native reopen before and after forced `.shm` rebuild observe that
  final CTAS state.
