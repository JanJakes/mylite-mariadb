# Ownerless Gap Lock Release Retry

## Problem Statement

Ownerless SQL coverage already proves that an InnoDB next-key/gap lock held by
one ownerless process blocks a peer insert into the protected gap. That proves
the shared lock mirror can observe the conflict, but it does not prove the gap
becomes writable again after the holder rolls back and the waiting peer's shared
wait state has been cleaned up.

This slice extends the focused gap-lock selector with a post-release retry from
a fresh ownerless peer. The test still requires the first insert attempt to time
out while the gap lock is held, then requires the same insert key to succeed
after the holder releases the transaction.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/innobase/lock/lock0lock.cc:752` through
  `mariadb/storage/innobase/lock/lock0lock.cc:855` decides record-lock wait
  compatibility for ordinary, gap, record-not-gap, and insert-intention modes.
- `mariadb/storage/innobase/lock/lock0lock.cc:6633` requests
  `LOCK_X | LOCK_GAP | LOCK_INSERT_INTENTION` for insert-intention waits.
- `mariadb/storage/innobase/row/row0sel.cc:4818` through
  `mariadb/storage/innobase/row/row0sel.cc:4832` places gap locks for locking
  reads.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc:2356`
  through `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc:2361`
  mirrors InnoDB `LOCK_GAP` and `LOCK_INSERT_INTENTION` flags into MyLite's
  directory-backed ownerless lock registry.

## Design

Extend `test_ownerless_gap_lock_blocks_insert()`:

1. one ownerless child runs a repeatable-read `SELECT ... FOR UPDATE` on a
   missing secondary-index value, holding the next-key/gap lock;
2. a peer ownerless child attempts to insert that key and must return MariaDB
   lock-wait timeout `1205`;
3. after the holder rolls back, the parent verifies the failed insert left no
   row;
4. a fresh ownerless peer inserts the same key successfully; and
5. the final ownerless observer verifies the inserted row and aggregate.

No product-code change is expected. A failure would indicate stale shared wait
state, missed release cleanup, or over-conservative ownerless conflict handling
after native InnoDB has released the gap lock.

## Scope And Non-Goals

In scope:

- Focused SQL coverage for gap-lock timeout followed by post-release insert
  success.
- Compatibility/spec wording that distinguishes blocking evidence from
  release/retry evidence.

Out of scope:

- Exhaustive next-key/gap-lock matrices across all search predicates.
- SQL-level table-lock fault injection; earlier negative proof did not find a
  SQL shape reaching the ownerless table-wait callback.
- Predicate/page locks for spatial indexes.

## Compatibility Impact

No new SQL syntax or user-visible behavior is added. The slice strengthens the
claim that ownerless cross-process lock mirroring preserves MariaDB/InnoDB
gap-lock blocking and release behavior for the tested secondary-index gap
shape.

## Directory, Storage, Build, And API Impact

No directory layout, native storage format, public API, build-profile, binary
size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `gap-lock`.
- Build and run focused `gap-lock` in `ownerless-test-hooks`.
- Run nearby transaction selectors:
  - `savepoint`
  - `serializable`
  - `write-skew`
- Run the ownerless SQL weighted CTest shard containing
  `test_ownerless_gap_lock_blocks_insert`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The first peer insert into the locked gap returns MariaDB lock-wait timeout.
- The failed insert leaves no row behind.
- After the holder rolls back, a fresh ownerless peer can insert the same key.
- A final ownerless observer sees exactly one inserted row and the expected
  aggregate.
- Existing savepoint and serializable selectors still pass.

## Risks And Unresolved Questions

- This covers one deterministic secondary-index gap-lock shape. Broader
  next-key/gap-lock predicate matrices remain future compatibility work.
- The test uses timeout for the first blocked insert, so it proves cleanup after
  timeout plus holder rollback rather than wake-before-timeout behavior for a
  still-waiting insert.
