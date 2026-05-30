# Ownerless Lock Before Grant Fault

## Problem Statement

Phase 9 crash coverage still calls out lock-grant fault injection. The
after-grant side is covered by `ownerless-lock-grant-fault`, but the earlier
path where a blocked writer has entered MariaDB's external ownerless record
wait and dies before local grant still needs deterministic coverage.

This slice adds a narrow MariaDB hook-shim callback at that pre-grant boundary
and SQL coverage proving recovery behavior.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/storage/innobase/lock/lock0lock.cc`
  - `lock_rec_lock()` calls MyLite's reserve-record hook before local grant.
    When the shared registry reports an external ownerless conflict,
    `mylite_ownerless_innodb_lock_enqueue_external_record_wait()` creates the
    local waiting record lock.
  - `mylite_ownerless_innodb_lock_wait_for_external_grant()` waits for the
    external ownerless conflict and later calls
    `mylite_ownerless_innodb_lock_try_grant_external_wait()`.
  - `lock_grant()` is therefore after the boundary this slice needs.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  - `mylite_ownerless_innodb_lock_before_external_record_wait()` is the new
    hook-shim point used by the local waiting-lock path before grant.
- `packages/libmylite/src/database.cc`
  - The new first-party before-record-wait callback is installed with the
    existing ownerless lock hook set and is a no-op except for the unsafe test
    pause.
  - `ownerless_process_owner_state_requires_recovery()` treats the killed
    writer's active transaction state as recovery-sensitive, so live-peer
    cleanup must return busy.

## Design

Add `mylite_ownerless_innodb_lock_before_record_wait_callback` to the ownerless
InnoDB lock hook set. The MariaDB hook shim calls it immediately before waiting
for an external record conflict. MyLite's implementation validates the callback
context and triggers `record-lock-before-grant` only in unsafe test builds.

Add SQL crash coverage that:

1. Holds row `id=1` locked by `SELECT ... FOR UPDATE` in an ownerless
   transaction.
2. Starts a writer that attempts to update the same row.
3. Pauses the writer after it reaches the external ownerless record wait and
   before MariaDB can grant the local waiting lock.
4. Kills the paused writer.
5. Proves a third ownerless opener returns `MYLITE_BUSY` while an idle live peer
   remains open.
6. Releases the holder, then reopens with no live peers and verifies the
   existing committed rows survived while the interrupted writer update did not
   apply.

## Scope

In scope:

- A narrow MariaDB hook-shim callback for external record wait entry.
- MyLite no-op callback implementation plus unsafe pause.
- Ownerless SQL crash coverage for live-peer busy and no-live rebuild behavior.
- Spec and compatibility updates for pre-grant record-lock fault coverage.

Out of scope:

- Changing lock-registry layout, conflict rules, or wait semantics.
- Table-lock wait fault injection.
- Group commit or redo publication behavior.

## Compatibility Impact

No public SQL or C API behavior changes. The hook callback is internal and
installed by `libmylite`; normal builds do not pause unless the unsafe test
fault environment is configured in a hooks-enabled build.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises existing ownerless volatile
state and no-live rebuild behavior inside the MyLite database directory.

## Native Storage Impact

The killed writer has not been granted the target record lock and must not
modify the row. The existing committed rows provide the durable data oracle.

## Binary Size Impact

Adds one internal hook callback pointer and one first-party no-op callback in
the embedded profile. No dependency or durable state is added.

## Test Plan

- Rebuild the MariaDB embedded archive after editing `mariadb/`.
- Build the `ownerless-test-hooks` SQL target.
- Run the new before-grant lock crash selector.
- Run the ownerless hook CTest filter covering cross-process SQL and negative
  proof tests.
- Rebuild and run the embedded ownerless SQL filter.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The writer is killed before MariaDB grants the local waiting record lock.
- Live-peer cleanup returns busy while the interrupted writer state remains.
- No-live reopen preserves the holder commit and drops the interrupted writer
  update.
- Existing ownerless hook and embedded SQL coverage remains green.

## Risks And Open Questions

- This covers record-lock external waits, not table-lock waits.
- The callback is an upstream-derived hook-shim extension; keep it narrow to
  control fork maintenance cost.
