# Ownerless Crash-Tail Isolation

## Problem

The `crash-tail` selector serially invokes many ownerless crash tests inside
one process. Most of the selector is available only in unsafe hook builds, with
one non-hook cleanup subcase kept at the end. Focused selectors and internal
per-test shard execution run
individual tests in child processes, but `crash-tail` previously let MariaDB
embedded and InnoDB process-global state accumulate across many forced crash
and recovery scenarios.

During generated-column FK action crash verification, `crash-tail` twice hit
an existing InnoDB purge assertion in `trx0purge.cc` while running
`ownerless-dictionary-ddl-finish-crash.mylite`. The same dictionary-finish
case passed in isolation, and a full `crash-tail` rerun passed. That points to
order-dependent process-global state rather than the focused selector under
test.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `run_ownerless_sql_test_case()` already runs ordinary ownerless SQL test
  cases in a child process and checks the child exit status from the parent.
- Before this slice, the same file's `crash-tail` selector directly called each
  hook crash test in sequence, so any process-global MariaDB/InnoDB state left
  behind by one selector remained in the parent process for the next selector.
- The repeated failing leftover directory was
  `ownerless-dictionary-ddl-finish-crash.mylite`; direct
  `--ownerless-sql-test-case=164` execution passed both times.

## Scope And Non-Goals

In scope:

- Run each `crash-tail` subcase in a fresh child process.
- Preserve the existing ordered `crash-tail` selector and its test contents.
- Keep focused selectors unchanged.
- Keep product code unchanged.

Out of scope:

- Changing MariaDB/InnoDB purge behavior.
- Changing ownerless recovery semantics.
- Adding or removing crash-tail coverage.
- Making `crash-tail` a CI job.

## Design

- Add a small test helper,
  `run_ownerless_crash_tail_test(ownerless_test_fn test_fn)`, next to the
  existing child process helpers.
- The helper forks, runs one crash-tail test function in the child, exits with
  status 0 if the function returns, and uses `wait_for_child()` in the parent.
- Replace the direct calls inside the `crash-tail` selector with calls through
  this helper.

## Compatibility Impact

No SQL, C API, storage, or runtime compatibility changes. This is test harness
isolation only.

## Directory And Lifecycle Impact

No product directory layout changes. The harness still creates one temporary
database directory per subcase; the parent process now discards embedded
process-global state between subcases by waiting for a child process exit.

## Native Storage Impact

No native storage format or runtime behavior changes. The slice reduces
cross-subcase contamination while preserving the same native crash/recovery
coverage.

## Binary Size And Dependencies

No production binary or dependency impact. The change is compiled only into the
test executable.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `ownerless-test-hooks`.
- Run `mylite_ownerless_cross_process_sql_test crash-tail`.
- Run focused hook selectors adjacent to the recent slices:
  `foreign-key-action-crash` and
  `generated-column-foreign-key-action-crash`.
- Run the hook ownerless negative-proof CTest label, embedded ownerless
  cross-process SQL CTest label, ownerless stress, `format-check`, `tidy`, and
  diff checks before commit.

## Acceptance Criteria

- `crash-tail` passes without the repeated dictionary-finish InnoDB purge
  assertion.
- A failing crash-tail subcase still fails the parent selector through
  `wait_for_child()`.
- No ownerless temp directories or ownerless test processes remain after the
  run.

## Risks And Follow-Up

- Per-subcase child isolation adds a small fork/wait overhead to `crash-tail`.
  That is acceptable for a hook-only release-gate selector.
- If the purge assertion still appears with per-subcase isolation, it is no
  longer explainable as crash-tail parent-process state contamination and must
  be investigated as a native recovery bug.
