# Ownerless Zombie Process Liveness

## Problem

Ownerless recovery decides whether volatile shared-memory state can be cleaned
by asking whether the process ID recorded in a process slot is still alive.
The POSIX `kill(pid, 0)` probe is not precise enough on Linux: an
exited-but-unreaped child still has a PID, so `kill(pid, 0)` succeeds while the
process is a zombie. A zombie process cannot run SQL, release ownerless locks,
or commit an active transaction, so treating it as live can keep recovery
blocked until the parent reaps it.

This was exposed by a local CI-shaped ownerless SQL loop timeout in
`test_ownerless_ddl_refreshes_peer_dictionary`: the spawned DDL child had
already exited as a zombie, while the parent process was still stuck closing
the ownerless runtime. The direct case and repeated focused runs passed, so
the bounded fix is the liveness predicate rather than a broad DDL rewrite.

## Source Findings

- MariaDB base ref: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::ownerless_process_is_alive()` used
  `kill(pid, 0)` and treated success as live.
- `allocate_concurrency_process_slot()` runs dead-owner cleanup through that
  predicate before assigning the current opener's ownerless process slot.
- Dictionary generation waits, shared-readonly peer checks, and explicit
  transaction live-owner checks also use the same predicate.
- Linux exposes process state through `/proc/<pid>/stat`; state `Z` means the
  process has exited and is waiting to be reaped.

## Design

Keep the existing portable liveness behavior for non-Linux platforms and for
Linux permission-denied cases. On Linux only, when `kill(pid, 0)` succeeds,
read `/proc/<pid>/stat` and parse the process state after the final `)` in the
command field. If the state is `Z`, return "not alive" to ownerless cleanup.
If `/proc` cannot be read or parsed, keep the conservative existing behavior
and treat the process as live.

Add ownerless SQL coverage that:

1. forks an ownerless writer,
2. commits an ownerless update,
3. exits without `mylite_close()`,
4. leaves the child unreaped until `/proc/<pid>/stat` reports zombie,
5. opens the same ownerless database directory, and
6. verifies the committed row is visible and the stale owner slot can be
   cleaned before the parent finally reaps the child.

## Compatibility Impact

No public API, SQL syntax, result-shape, file-format, or directory-layout
behavior changes. The liveness definition is tightened for Linux ownerless
cleanup so an exited process does not continue to block no-live recovery merely
because its parent has not yet called `waitpid()`.

## Directory And Lifecycle Impact

No new files or shared-memory records are introduced. Existing dead-owner
cleanup paths that do not require native crash recovery can now run before
parent reap for Linux zombie owners.

## Native Storage Impact

The test exercises native InnoDB visibility for a committed writer that exits
without clean MariaDB shutdown. Uncommitted zombie-owner rollback while live
peers remain is still covered by the broader dead-owner recovery policy and is
not claimed by this liveness slice.

## Test And Verification Plan

- Build production `mylite_ownerless_cross_process_sql_test`.
- Run the direct `zombie-writer-cleanup` selector.
- Run the registered `sql-case` for the new test.
- Re-run the previously observed DDL-refresh case and a nearby window.
- Run a CI-shaped full ownerless SQL loop to check the reproduced timeout is
  no longer present.
- Run production build guards, `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- Linux zombie process slots are treated as dead for ownerless liveness.
- Non-Linux behavior and unreadable `/proc` cases remain conservative.
- The new SQL regression proves stale owner cleanup can proceed before the
  parent reaps an exited ownerless writer when no native recovery state remains.
- Focused and CI-shaped ownerless SQL runs pass with production artifacts.

## Risks And Follow-Up

- This does not by itself prove every DDL shutdown race is fixed; it removes one
  real liveness ambiguity observed in timeout diagnostics.
- Broader native redo/checkpoint reconciliation, DDL/file lifecycle recovery,
  active-reader pressure policy, and external stress remain separate ownerless
  completion gaps.
