# Ownerless Lock Grant Fault

## Problem Statement

Phase 9 crash coverage still calls out lock-grant fault injection as planned.
Ownerless InnoDB locking is cross-process state: if a writer dies after a
blocked record lock is granted and published to the shared registry, a live
peer must not delete that state as ordinary stale-owner cleanup. Once no
ownerless peers remain, reopening the directory should rebuild volatile
coordination state and preserve only committed updates.

This slice adds deterministic unsafe-hook coverage for that granted-record-lock
boundary.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/storage/innobase/lock/lock0lock.cc`
  - `lock_rec_lock()` calls
    `mylite_ownerless_innodb_lock_reserve_record_for_grant()` before creating
    or extending a local record lock, so MyLite can reject cross-process
    conflicts before MariaDB grants the local lock.
  - `lock_reset_lock_and_trx_wait()` clears MariaDB wait state and calls
    `mylite_ownerless_innodb_lock_clear_transaction_wait()`.
  - `lock_grant()` clears `LOCK_WAIT`, then calls
    `mylite_ownerless_innodb_lock_publish_record_bits()` for record locks
    before resuming the waiting transaction.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  - `mylite_ownerless_innodb_lock_publish_record_bit()` forwards granted
    record locks through the first-party acquire-record callback.
  - `mylite_ownerless_innodb_lock_wait_for_external()` and
    `mylite_ownerless_innodb_lock_wait_until_record_available()` cover the
    blocked external-wait path before a later local grant.
- `packages/libmylite/src/database.cc`
  - `ownerless_innodb_lock_acquire_record_hook()` records granted record locks
    in the directory-backed InnoDB lock registry.
  - `ownerless_process_owner_state_requires_recovery()` treats active InnoDB
    lock-registry entries as recovery-sensitive, so live-peer cleanup returns
    busy instead of deleting them.

## Design

Add an unsafe-test-only pause point after
`ownerless_innodb_lock_acquire_record_hook()` successfully publishes a record
lock to the shared registry. The pause is generic at the acquire-record hook
boundary, but the SQL test forces it to occur through MariaDB's `lock_grant()`
path:

1. A holder process starts an ownerless transaction and updates row `id=1`.
2. A writer process attempts to update the same row and waits on the external
   ownerless record lock.
3. The holder commits, letting the writer's wait path call `lock_grant()`.
4. The writer pauses after MyLite publishes the newly granted record lock and
   before the SQL update can finish.
5. The parent kills the paused writer.
6. A live idle ownerless peer remains open and a third opener must receive
   `MYLITE_BUSY`.
7. After all peers exit, a no-live reopen rebuilds volatile coordination and
   verifies the holder commit survived while the interrupted writer update did
   not apply.

The slice intentionally avoids editing MariaDB source. The existing
MariaDB-derived hook call in `lock_grant()` already reaches the MyLite-owned
callback boundary needed for deterministic coverage.

## Scope

In scope:

- Unsafe pause after successful shared record-lock publication.
- SQL crash coverage for the blocked-writer grant path.
- Spec and compatibility updates marking after-grant record-lock crash coverage.

Out of scope:

- Changing lock-registry layout, conflict rules, or cleanup policy.
- Adding a new MariaDB hook point.
- Covering the pre-grant waiting-lock publication boundary; that remains a
  separate fault-injection slice.

## Compatibility Impact

No public SQL or C API behavior changes. The expected compatibility behavior is
that a killed writer does not apply an interrupted row update, while committed
peer updates remain visible after ownerless recovery.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises the existing volatile
`concurrency/mylite-concurrency.shm` InnoDB lock-registry segment and the
existing no-live rebuild path.

## Native Storage Impact

The killed writer dies after receiving the record lock but before its SQL update
can complete and commit. The holder transaction's committed native state must
remain durable; the interrupted writer must not contribute committed page
versions.

## Binary Size Impact

Normal builds are unchanged. The pause point is guarded by
`MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS`.

## Test Plan

- Build the `ownerless-test-hooks` SQL target.
- Run the new lock-grant crash selector.
- Run the ownerless hook CTest filter covering cross-process SQL and negative
  proof tests.
- Run the embedded ownerless SQL filter to confirm normal builds still pass.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The writer waits behind a peer-held ownerless record lock before the fault.
- The writer is killed after shared record-lock publication from the grant path.
- Live-peer cleanup returns busy while the killed writer's recovery-sensitive
  state is present.
- No-live reopen preserves the holder commit and drops the interrupted writer
  update.
- Existing ownerless hook and embedded SQL coverage remains green.

## Risks And Open Questions

- The pause is implemented at the shared acquire-record callback. The test
  constrains that callback to MariaDB's `lock_grant()` path; direct successful
  no-wait lock acquisition is not the evidence this slice claims.
- Pre-grant waiting-lock publication still needs its own deterministic crash
  test.
