# Ownerless Prepared-Result Pressure Pin

## Problem Statement

Ownerless active-reader pressure coverage proves the soft WAL limit blocks
direct and prepared writes while an explicit repeatable-read transaction pins
page-version WAL. Prepared result cursors are a second live-reader class: an
ownerless prepared `SELECT` can keep its page-version read pin until the cursor
is exhausted, reset, or finalized. Timer checkpoint coverage proves such a
cursor gates background reclaim, but the public pressure-limit path also needs
focused evidence that a cursor-held pin is enough to throttle a separate
writer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/include/mysql.h` exposes the embedded prepared-statement lifecycle
  used by `mysql_stmt_execute()`, `mysql_stmt_fetch()`, and
  `mysql_stmt_close()`. MyLite wraps that lifecycle in
  `packages/libmylite/src/database.cc:mylite_step()`,
  `mylite_reset()`, and `mylite_finalize()`.
- `packages/libmylite/src/database.cc` publishes handle-owned page-version
  pins through `ensure_ownerless_handle_page_version_pin()` and releases them
  through `release_ownerless_handle_page_version_pin()`.
- `packages/libmylite/src/database.cc:mylite_step()` transfers ownerless
  prepared result activity to the statement handle while rows remain available,
  and result cleanup releases that activity when the cursor is exhausted,
  reset, or finalized.
- `packages/libmylite/src/database.cc:read_ownerless_pressure_state()` counts
  active page-version pins from the shared page-pin registry and stats
  `concurrency/mylite-concurrency.wal`.
- `packages/libmylite/src/database.cc:enforce_ownerless_page_log_limit_policy()`
  releases only the caller's transient handle read pin before checking the
  shared pressure state, then returns `MYLITE_BUSY` before MariaDB write
  execution when active pins retain WAL at or above the configured limit.

## Design

Add a focused ownerless SQL selector,
`active-reader-pressure-prepared-result-pin`, and a matching CTest.

The selector:

1. Starts from the normal ownerless InnoDB fixture and checkpointed WAL.
2. Holds an older repeatable-read snapshot in a child process.
3. Commits one ownerless update, forcing retained page-version WAL.
4. Opens a second ownerless handle, executes a prepared `SELECT`, fetches one
   row, and leaves the result cursor open.
5. Verifies the pressure status sees both active pins.
6. Releases the older transaction pin and verifies the prepared cursor remains
   as the only active page-version pin while WAL is still retained.
7. Opens a separate pressure-limited ownerless writer with the limit set to the
   retained WAL size.
8. Verifies direct and prepared `UPDATE` return `MYLITE_BUSY`, set no MariaDB
   errno, and leave row data unchanged.
9. Finalizes the prepared result cursor, opens a fresh limited writer, and
   verifies the same update succeeds because no active pin remains.
10. Verifies final data through ownerless/native reopen after forced `.shm`
    rebuild and checks retained WAL is checkpointed or reduced to
    native-support-only proof records.

## Scope And Non-Goals

In scope:

- Prepared result cursor as an active page-version pin source for the public
  pressure-limit policy.
- Direct and prepared write rejection before MariaDB execution while that
  cursor pin is the remaining active pin.
- Post-finalize retry and reopen evidence.

Out of scope:

- New pressure policy behavior, public APIs, background checkpoint scheduling,
  or cursor cancellation.
- Broader DDL/file-lifecycle recovery, SQL-level table-lock fault injection,
  and external MariaDB/RQG stress.

## Compatibility Impact

No SQL semantics or public API behavior changes. The slice adds evidence for an
existing MyLite ownerless resource policy: an opted-in writer can receive
`MYLITE_BUSY` while any active page-version reader class retains WAL at the
configured soft limit.

## Directory And Lifecycle Impact

No directory layout changes. The test observes existing ownerless files under
`concurrency/`, including `.wal`, `.ckpt`, and `.shm`, and verifies forced
shared-memory rebuild after the cursor releases.

## Native Storage Impact

No native InnoDB format changes. The test depends on existing page-version WAL,
page-pin registry, and native checkpoint proof behavior.

## Public API, Build, Size, And Dependencies

No public API, build profile, binary-size, license, or dependency changes. The
slice adds one focused SQL selector, one CTest entry, and documentation.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-prod`.
- Run direct selector `active-reader-pressure-prepared-result-pin`.
- Run focused CTests for the new selector plus adjacent
  `active-reader-pressure-limit`, `active-reader-pressure-diagnostics`, and
  `timer-checkpoint-scheduling`.
- Run the hook ownerless primitive/SQL subset when the focused selector passes.
- Run ownerless stress, `format-check`, and `git diff --check`.

## Acceptance Criteria

- A prepared result cursor keeps a page-version pin after the older snapshot
  reader releases.
- A separate ownerless writer with `ownerless_page_log_limit_bytes` set to the
  retained WAL size reports the pressure limit reached.
- Direct and prepared writes return `MYLITE_BUSY` before changing data while
  the cursor pin is live.
- Finalizing the cursor removes the active pin, lets the same logical update
  succeed, and permits WAL reclamation.
- Ownerless and ordinary native reopen observe the final committed data.

## Risks And Unresolved Questions

- The test is deterministic and focused; it does not replace long-running
  external MariaDB/RQG pressure stress.
- The public limit remains a soft pre-dispatch cap. A write that begins below
  the cap may still grow retained WAL past the cap before the next writer is
  rejected.
