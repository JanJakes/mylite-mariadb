# Ownerless Native Table-Wait Reachability

## Problem

Ownerless table-lock coverage had a narrow primitive proof and a SQL negative
proof, but no supported SQL statement had been shown to reach the native
InnoDB external table-wait path. The obvious SQL locked-table route remains
unsupported because ownerless `LOCK TABLES`/`UNLOCK TABLES` is rejected before
MariaDB execution.

The remaining bounded question is whether a supported SQL shape can publish a
shared ownerless native table-wait entry without enabling SQL locked-table
mode.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0sel.cc` takes an InnoDB intention table
  lock for locking reads such as `SELECT ... FOR UPDATE`.
- `mariadb/storage/innobase/row/row0ins.cc` can request a table `LOCK_X` for
  the empty-page bulk-insert path when the target clustered root page is empty,
  the table has no record locks, the statement is not checking foreign keys,
  and secondary unique checks are disabled.
- `mariadb/storage/innobase/lock/lock0lock.cc:lock_table_low()` first checks
  local InnoDB table-lock queues, then asks
  `mylite_ownerless_innodb_lock_reserve_table_for_grant()`. A cross-process
  conflict becomes an external waiting table lock through
  `mylite_ownerless_innodb_lock_enqueue_external_table_wait()`.
- `mariadb/storage/innobase/lock/lock0lock.cc` later calls
  `mylite_ownerless_innodb_lock_wait_for_external()` for the waiting lock.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  dispatches external table waits to MyLite's wait-until-table hook, which
  waits through
  `mylite_ownerless_innodb_lock_registry_wait_until_table_available()`.

## Design

Add a focused unsafe-hook selector in the ownerless cross-process SQL harness:

1. Create an empty InnoDB table.
2. Hold a peer ownerless transaction open with
   `SELECT * FROM ... LOCK IN SHARE MODE` against that empty table, creating a
   compatible InnoDB shared locking read without adding rows.
3. In a second ownerless process, set `foreign_key_checks=0` and
   `unique_checks=0`, then attempt the first insert into the empty table so
   MariaDB's empty-page bulk path requests table `LOCK_X`.
4. Poll `concurrency/mylite-concurrency.shm` and scan the ownerless InnoDB lock
   registry slots for a `WAITING` entry whose kind is `TABLE`.
5. Release the holder.
6. Assert the writer completes, the table-wait slot count returns to zero, a
   retry insert succeeds, and ownerless/native reopen before and after forced
   `.shm` rebuild reads the final rows.

Keep the existing SQL negative-proof selector unchanged. It still proves the
representative blocked DDL shapes do not reach the older local
`table-lock-wait` callback, and ownerless SQL locked-table mode stays rejected.

## Compatibility Impact

No public SQL behavior changes. The covered SQL shape is ordinary InnoDB
locking-read plus insert behavior under ownerless read/write opens. Ownerless
`LOCK TABLES` and `UNLOCK TABLES` remain unsupported until connection-scoped
locked-table mode has a separate design.

## Directory And Lifecycle Impact

No durable directory-layout changes. The test observes the existing
directory-owned ownerless InnoDB lock registry in
`concurrency/mylite-concurrency.shm` and verifies the wait entry is cleared
after the blocking transaction releases.

## Native Storage Impact

No native storage format changes. The test uses MariaDB-native InnoDB locks and
the existing MyLite ownerless table-lock registry.

## Build And Performance Impact

Production builds are unchanged. The new CTest only runs in the
`ownerless-test-hooks` preset.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused `native-table-wait` selector.
- Run the existing `table-lock-wait-negative-proof` selector.
- Run the `ownerless-test-hooks` negative-proof CTest label.
- Build the production embedded target to verify the unsafe hook remains
  compiled out there.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- The focused selector proves a supported SQL shape publishes a shared
  ownerless InnoDB table-wait slot while the holder transaction is still live.
- The table-wait slot count returns to zero after release.
- The blocked insert and a retry insert both persist and remain visible through
  ownerless reopen, native reopen, and forced `.shm` rebuild.
- Existing SQL negative-proof coverage continues to pass.

## Risks And Follow-Up

- This covers the external native table-wait registry path, not SQL
  locked-table mode. `LOCK TABLES` remains rejected.
- The test is intentionally deterministic and hook-gated; broader randomized
  native table-wait stress remains future work.
