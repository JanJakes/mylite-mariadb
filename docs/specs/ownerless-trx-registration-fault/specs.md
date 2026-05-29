# Ownerless Transaction Registration Fault

## Problem Statement

Phase 9 crash coverage includes redo reservation, redo write, latest-LSN,
page-visible, checkpoint, and DDL fault points, but it does not yet isolate the
earlier state where an ownerless writer has entered the shared transaction
registry and dies before the SQL update can proceed. That state must not be
silently cleaned while another ownerless peer is live, because the shared
transaction registry is part of the cross-process recovery evidence.

This slice adds deterministic unsafe-hook coverage for a crash immediately after
MyLite publishes an ownerless read-write transaction in shared coordination
state.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/storage/innobase/include/trx0sys.h`
  - `trx_sys_t::register_rw()` calls `mylite_ownerless_trx_register()` before
    assigning `trx->id`, inserting the transaction into MariaDB's local
    `rw_trx_hash`, and refreshing the local hash version.
  - `trx_sys_t::deregister_rw()` removes the local transaction and calls
    `mylite_ownerless_trx_deregister()`.
- `mariadb/storage/innobase/trx/mylite_ownerless_trx_hooks.cc`
  - The hook shim stores first-party callbacks and forwards
    `mylite_ownerless_trx_register()` into the current MyLite runtime context.
- `packages/libmylite/src/database.cc`
  - `ownerless_trx_register_hook()` publishes the transaction in the
    directory-backed transaction registry through
    `mylite_ownerless_trx_registry_begin()`.
  - `ownerless_process_owner_state_requires_recovery()` treats active
    transaction-registry entries as recovery-sensitive state, so live peers
    must not clear them.
  - No-live recovery can rebuild volatile `.shm` state once all owners are gone.

## Design

Add an unsafe-test-only pause point after `ownerless_trx_register_hook()`
successfully creates the shared transaction-registry entry and before it returns
to MariaDB. The hook is only compiled in the `ownerless-test-hooks` preset.

Add SQL crash coverage that:

1. Opens an idle ownerless peer so the database still has a live owner.
2. Starts a writer that pauses after shared transaction registration while
   executing an update.
3. Kills the paused writer.
4. Proves a third live ownerless opener receives `MYLITE_BUSY` because the dead
   writer's active transaction entry is recovery-sensitive.
5. Releases the idle peer.
6. Reopens ownerless read/write with no live peers and verifies the interrupted
   update was not applied.

The slice does not change product semantics; it only adds a deterministic fault
point and coverage around existing recovery decisions.

## Scope

In scope:

- Unsafe test pause after shared transaction registration.
- Ownerless SQL test for live-peer busy and no-live rebuild behavior.
- Compatibility/spec updates that mark the transaction-registration fault point
  as covered.

Out of scope:

- Changing transaction registry layout or recovery policy.
- Adding MariaDB upstream-derived hook surfaces.
- Commit publish fault points after page visibility; those remain separate
  crash-hardening slices.

## Compatibility Impact

No SQL or public API behavior changes. The expected compatibility behavior is
that an interrupted uncommitted ownerless write is not visible after recovery,
and live peers avoid deleting recovery-sensitive transaction state.

## Directory And Lifecycle Impact

No new files or directory layout changes. The test exercises existing volatile
state in `concurrency/mylite-concurrency.shm` and the existing no-live rebuild
path.

## Native Storage Impact

The fault occurs before the SQL update can proceed past shared transaction
registration. No committed page-version WAL or durable native table change is
expected from the interrupted writer.

## Binary Size Impact

Normal builds are unchanged. The pause point is guarded by
`MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS`.

## Test Plan

- Build the `ownerless-test-hooks` SQL target.
- Run the new selector for the transaction-registration crash.
- Run the ownerless hook CTest filter covering cross-process SQL and negative
  proof tests.
- Run the embedded ownerless SQL filter to confirm normal builds still pass.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The paused writer is killed after the shared transaction-registry entry is
  active.
- Live peer cleanup returns busy instead of deleting that entry.
- No-live ownerless reopen rebuilds volatile state without applying the
  interrupted update.
- Existing ownerless hook and embedded SQL coverage remains green.

## Risks And Open Questions

- This covers the MyLite transaction-registry boundary, not every MariaDB local
  transaction state transition after `trx_sys_t::register_rw()` returns.
- Later slices still need explicit coverage for before/after lock grant and
  before/after commit publish points.
