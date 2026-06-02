# Ownerless Stress Child Failure Cleanup

## Problem

The opt-in ownerless stress selectors launch several child processes over one
database directory. The previous parent-side wait pattern asserted on the first
nonzero child exit. If one worker failed while siblings still held CTest output
descriptors open, the real failure could be hidden behind the stress test's
900-second timeout and leave ownerless test processes running until the timeout
or manual cleanup.

This slice makes stress worker failure reporting deterministic without changing
production ownerless SQL behavior.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- No MariaDB source path changes are required. This slice is limited to the
  MyLite C test harness and CMake test registration.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` uses `fork()`,
  `waitpid()`, pipe barriers, and `_exit()` for ownerless stress workers.
- POSIX `waitpid(pid, ..., WNOHANG)` lets the parent poll specific child PIDs
  without blocking behind a still-running sibling after another child has
  already failed.
- POSIX `kill(pid, SIGKILL)` is acceptable for a failing test harness cleanup
  path because the test is already failing and the priority is to reap siblings
  and preserve the first failure status.

## Scope And Non-Goals

In scope:

- Add a reusable multi-child collector for ownerless stress tests.
- Terminate and reap still-running stress siblings after the first child
  failure.
- Report child index, PID, exit status, and signal status for each failed or
  killed child.
- Add a focused passing regression selector that intentionally creates one
  failing child and one long-lived sibling, then verifies the collector returns
  failure after cleanup.
- Use the collector in the opt-in ownerless stress families that launch worker
  arrays.

Out of scope:

- Treating duplicate-key, deadlock, timeout, or aggregate-oracle failures as
  successful stress outcomes.
- Changing ownerless locking, page-version, redo, or recovery behavior.
- Replacing every focused two-process test's simple `wait_for_child()` path.

## Design

Add `wait_for_children_result(label, children, count)`.

The helper polls each tracked child with `waitpid(..., WNOHANG)`. A nonzero
exit, signal death, already-reaped child, or `waitpid()` error marks the group
failed. On the first failure, the helper sends `SIGKILL` to every tracked child
that has not yet exited, continues polling until all children are reaped, and
returns `MYLITE_TEST_CHILD_EXEC_FAILED`. `wait_for_children()` wraps that helper
for stress tests that should assert all children succeeded.

The `child-failure-cleanup` selector forks a long-lived child and a failing
child, calls `wait_for_children_result()`, and passes only if the helper reports
failure after cleaning up the long-lived sibling.

## Compatibility Impact

No public SQL, C API, or storage behavior changes. The compatibility evidence
improves because ownerless stress failures now fail fast with the failing child
status instead of being masked by leaked sibling processes and a CTest timeout.

## Directory And Lifecycle Impact

No database-directory layout changes. In failing stress runs, orphaned test
processes should no longer keep temporary MyLite directories open until CTest
timeout.

## Native Storage Impact

No native format or MariaDB storage-engine behavior changes.

## Binary Size And Dependencies

No production binary-size or dependency impact. The slice changes test code and
CMake test registration only.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in the ownerless-stress
  preset.
- Run `child-failure-cleanup`.
- Run focused `fk-graph-stress` with the stress preset environment.
- Run a bounded repeated FK graph stress loop to confirm failures are no longer
  converted into orphaned-worker timeouts.
- Run the full `ownerless-stress` preset.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- The focused cleanup selector passes and leaves no ownerless worker processes.
- Opt-in ownerless stress worker arrays use the multi-child collector.
- A child failure reports the child index/PID/status and reaps all siblings.
- Passing stress selectors continue to pass.
- Documentation and compatibility notes describe the harness behavior without
  claiming new production concurrency semantics.

## Risks And Follow-Up

- This is a harness reliability fix; it does not explain or fix any underlying
  FK duplicate-key or aggregate-oracle failure if one reappears.
- The helper intentionally uses `SIGKILL` only after a test child has already
  failed. That keeps failure cleanup deterministic but means a failing run may
  still require normal temporary-directory cleanup by the test runner.
