# Ownerless Native Table-Wait Crash Coverage

## Problem

Ownerless lock fault coverage already proved record-wait crash paths and
primitive table-wait owner cleanup. The remaining table-wait SQL evidence was
weaker: the hook build proved that a reachable native InnoDB table wait can be
published from SQL and later cleared, but it did not kill the SQL waiter while
the shared table-wait entry was live.

This slice adds that crash proof for the reachable native table-wait SQL path
by killing the blocked SQL process after its shared registry wait is observable.
It does not claim support for SQL locked-table mode or for DDL shapes that
current negative coverage proves stop before MyLite's local table-wait
callback.

## Source Findings

- MariaDB base: MariaDB 11.8 LTS as imported by this tree.
- `mariadb/storage/innobase/lock/lock0lock.cc`:
  `lock_table_enqueue_waiting()` creates a waiting table lock with
  `lock_table_create(... LOCK_WAIT ...)` and calls
  `mylite_ownerless_innodb_lock_publish_table_wait()`.
- `mariadb/storage/innobase/lock/lock0lock.cc`: `lock_table_low()` also routes
  MyLite external table-lock conflicts through
  `mylite_ownerless_innodb_lock_enqueue_external_table_wait()` after
  `mylite_ownerless_innodb_lock_reserve_table_for_grant()` returns
  `DB_LOCK_WAIT_TIMEOUT`.
- `packages/libmylite/src/database.cc`:
  `ownerless_innodb_lock_wait_table_hook()` is the local table-wait callback
  armed by the negative DDL proof, but the reachable empty-table insert shape
  uses the external table-availability path through
  `ownerless_innodb_lock_wait_until_table_hook()`.
- `packages/libmylite/src/ownerless_innodb_lock_registry.cc`:
  `mylite_ownerless_innodb_lock_registry_wait_until_table_available()` enters
  the common `wait_until_lock_available()` path, which publishes a shared
  waiting slot while the table lock is unavailable.

## Design

Add a hook-build SQL test that uses the existing shared registry visibility as
the synchronization point:

1. Creates an empty InnoDB table in ownerless mode.
2. Holds a peer `SELECT ... LOCK IN SHARE MODE` transaction over the empty
   table.
3. Starts a second ownerless process that runs the known reachable
   `foreign_key_checks=0` and `unique_checks=0` insert shape.
4. Polls `mylite-concurrency.shm` until the table-wait slot is observable.
5. Kills the second process while the shared table-wait slot remains live.
6. Verifies the shared table-wait entry remains observable after death.
7. Verifies a live-peer ownerless reopen returns busy while the blocking reader
   is still alive.
8. Kills the blocking reader, reopens ownerless read/write, and verifies
   no-live recovery removes the dead wait entry.
9. Confirms the interrupted insert is absent, retries the insert, and verifies
   ownerless/native reopen plus forced `.shm` rebuild.

## Compatibility Impact

This is coverage-only for the ownerless native InnoDB table-wait path. It
improves evidence for cross-process lock cleanup and retry behavior but does
not change SQL compatibility claims. Ownerless `LOCK TABLES` and
`UNLOCK TABLES` remain explicitly unsupported until SQL locked-table lifecycle
semantics are designed. Representative DDL wait shapes remain negative-proofed
as not reaching this callback.

## Directory And Lifecycle Impact

No durable file format changes are introduced. The shared wait entry lives in
the existing MyLite-owned `concurrency/mylite-concurrency.shm` coordination
file. No-live ownerless recovery rebuilds volatile coordination state and keeps
only committed native InnoDB data.

## Test Plan

- Add `libmylite.ownerless-native-table-wait-crash` under the
  `ownerless-test-hooks` preset.
- Run the new selector directly:
  `ctest --preset ownerless-test-hooks -R 'libmylite\.ownerless-native-table-wait-crash$' --output-on-failure`
- Rerun neighboring table-wait and lock hook coverage:
  `ctest --preset ownerless-test-hooks -R 'libmylite\.(ownerless-native-table-wait|ownerless-table-wait-negative-proof|embedded-ownerless-innodb-lock-hooks|ownerless-primitives)$' --output-on-failure`
- Run formatting/static checks relevant to the touched files.

## Acceptance Criteria

- The new test deterministically kills a SQL process after a native table-wait
  shared slot is published.
- The shared table-wait entry is observable after process death.
- Live-peer ownerless reopen remains busy while the blocking reader is alive.
- Ownerless no-live reopen removes the dead wait entry after the blocker dies.
- The interrupted insert is absent, retry succeeds, and ownerless/native reopen
  plus forced `.shm` rebuild see the committed rows.
- Compatibility docs continue to mark broader SQL table-lock wait fault
  injection and ownerless SQL locked-table mode as planned/unsupported.
