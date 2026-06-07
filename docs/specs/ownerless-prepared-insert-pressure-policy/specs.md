# Ownerless Prepared Insert Pressure Policy

## Summary

Ownerless active-reader pressure policy must throttle prepared
`INSERT ... SELECT` statements before MariaDB execution, not only direct writes
or a prepared `UPDATE`.

## Problem

The pressure-limit selector already proves direct write throttling and a
prepared `UPDATE` retry after a live repeatable-read snapshot pin drains. The
policy itself is token based and runs from both direct `mylite_exec()` and
prepared `mylite_step()` dispatch paths, so a prepared insert-select spelling
needs direct evidence.

Without this proof, MyLite could regress prepared `INSERT ... SELECT` pressure
handling while still passing the direct insert-select and prepared-update
cases.

## Source References

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_parse.cc` marks `SQLCOM_INSERT_SELECT` with
  `CF_CHANGES_DATA` and `CF_INSERTS_DATA`.
- `mariadb/sql/sql_lex.h` includes `SQLCOM_INSERT_SELECT` among DML commands
  handled as write-relevant operations.
- `packages/libmylite/src/database.cc` calls
  `enforce_ownerless_page_log_limit_policy()` from both prepared
  `mylite_step()` and direct `mylite_exec()` before ownerless statement locks
  and MariaDB execution.

## Scope And Non-Goals

In scope:

- Extend `active-reader-pressure-limit` with a prepared
  `INSERT INTO ... SELECT ...` statement.
- Require the first `mylite_step()` to return `MYLITE_BUSY` while retained WAL
  is at the configured active-reader soft cap.
- Verify the blocked prepared insert leaves row count and aggregate unchanged.
- Retry the same prepared insert after the reader releases and verify
  ownerless/native reopen after forced `.shm` rebuild.

Out of scope:

- Exhaustive prepared write-spelling matrices.
- New public API or pressure-policy behavior.
- External randomized pressure stress.

## Design

Reuse the existing pressure-limit flow:

1. Hold a repeatable-read ownerless snapshot pin in a child process.
2. Commit one ownerless update so retained page-version WAL exceeds the empty
   log header while the pin remains live.
3. Reopen with `ownerless_page_log_limit_bytes` set to the retained WAL size.
4. Keep the existing direct `UPDATE` and prepared `UPDATE` busy assertions.
5. Add a prepared `INSERT INTO app.ownerless_sql SELECT 3, 30` assertion that
   returns `MYLITE_BUSY` before mutation.
6. Release the reader, retry both prepared statements successfully, and verify
   the final table has 3 rows with `SUM(value)=62` through native reopen after
   forced `.shm` rebuild.

## Compatibility Impact

No default SQL behavior changes. The slice broadens evidence for the existing
opt-in ownerless page-version WAL pressure limit across prepared write
dispatch. The pressure policy remains a MyLite resource policy and returns
`MYLITE_BUSY` with MariaDB errno zero.

## Database Directory And Lifecycle Impact

No directory layout changes. The selector still verifies close-time WAL
checkpointing and forced shared-memory rebuild after pressure clears.

## Native Storage Impact

No native storage format changes. The blocked prepared insert-select must not
enter native InnoDB mutation paths while pressure is active; the same prepared
statement succeeds after the snapshot pin drains and native checkpoint
reclamation runs.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact. This is test and documentation coverage only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `active-reader-pressure-limit` in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run adjacent `active-reader-pressure-write-policy` and
  `active-reader-pressure-diagnostics` selectors.
- Run the relevant embedded ownerless SQL CTest shard.
- Run focused ownerless active-reader pressure stress.
- Run `format-check`, `git diff --check`, and cached diff checks.

## Acceptance Criteria

- Direct `UPDATE`, prepared `UPDATE`, and prepared `INSERT ... SELECT` return
  `MYLITE_BUSY` while retained WAL is at the configured soft cap.
- The blocked prepared insert leaves the table at 2 rows with `SUM(value)=31`.
- After the reader releases, both prepared statements retry successfully and
  leave 3 rows with `SUM(value)=62`.
- Native exclusive reopen after forced `.shm` rebuild observes the final state.

## Risks And Follow-Up

- This remains focused prepared insert-select coverage, not an exhaustive
  prepared write matrix.
- Full randomized external pressure stress remains planned separately.
