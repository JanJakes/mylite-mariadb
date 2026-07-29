# Ownerless Random Savepoint Same-Table Schedule

## Problem Statement

Ownerless transaction coverage now proves focused savepoint handoff for
independent tables, same-page writes, large same-table rows, and same-row
conflicts. The remaining transaction gap still includes broader same-table
schedules where several processes interleave explicit transactions, savepoints,
savepoint rollbacks, full transaction rollbacks, row-lock retries, and final
commits against the same InnoDB table.

This slice adds a bounded deterministic pseudo-random schedule. It is small
enough for normal ownerless SQL CI but forces overlapping row sets across three
processes, so final state proves retryable waits/deadlocks did not leak
rolled-back post-savepoint or transaction state.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/transaction.cc:711-748` implements
  `trans_rollback_to_savepoint()` and delegates storage-engine rollback through
  `ha_rollback_to_savepoint()`.
- `mariadb/sql/handler.cc:3382-3440` iterates participating engines for
  savepoint rollback.
- `mariadb/storage/innobase/handler/ha_innodb.cc:2920-2933` records the InnoDB
  savepoint undo number, and
  `mariadb/storage/innobase/handler/ha_innodb.cc:5026-5069` rolls back to that
  undo number.
- `mariadb/storage/innobase/lock/lock0lock.cc:748-872` covers record-lock wait
  checks used when overlapping ownerless writers collide on rows in different
  orders.

## Scope And Non-Goals

In scope:

- A Linux ownerless SQL selector named
  `random-savepoint-same-table-schedule`.
- Three ownerless writer processes over one five-row InnoDB table.
- Deterministic overlapping row schedules with `SAVEPOINT`,
  `ROLLBACK TO SAVEPOINT`, full `ROLLBACK`, retryable `1205`/`1213` handling,
  and committed final-state oracles.
- Ownerless reopen, forced `.shm` rebuild, ordinary native reopen, and no-live
  marker/WAL drain checks.

Out of scope:

- Crashes inside native InnoDB rollback/savepoint rollback internals.
- Exhaustive same-table deadlock fairness or unbounded stress.
- External MariaDB/RQG randomized replay.

## Design

Add `test_ownerless_random_savepoint_same_table_schedule()` to
`mylite_ownerless_cross_process_sql_test`.

Each writer runs six transactions. Every transaction updates one row, creates a
savepoint, updates a second row, sometimes rolls back to the savepoint, updates
a third row, and then either commits or fully rolls back. Row order differs by
worker and round, including reversed row orders that can produce row-lock waits
or deadlocks. Retryable `1205` and `1213` errors roll back the whole attempt
and retry the deterministic transaction. After a successful round, the worker
leaves an acquisition window longer than the configured one-second lock wait
before starting its next transaction. This keeps the bounded schedule focused
on wait/retry and rollback correctness instead of making exhaustive unfair-lock
starvation an accidental acceptance criterion.

All committed updates are additive, so the final oracle is independent of
commit order. The test checks row count, value sum, version sum, and weighted
value sum through ownerless reopen, ordinary native reopen, forced shared-memory
rebuild, and a final native reopen.

Verification of this slice also exposed that the older explicit transaction
stress worker treated MariaDB-compatible `1205`/`1213` outcomes as fatal even
though neighboring ownerless stress workers already retry those outcomes. The
same change gives `tx-stress` a bounded attempt loop; failed attempts roll back
the worker-owned table transaction and retry, preserving the deterministic
aggregate oracle.

## Compatibility Impact

No SQL feature or API behavior changes. This is additional compatibility
evidence for MariaDB explicit transaction, savepoint, row-lock wait, and
rollback behavior under ownerless cross-process writes.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The test exercises existing
ownerless wait/retry handling, page-version WAL retention, DML native
file-operation marker cleanup, forced `.shm` rebuild, and ordinary native
startup after ownerless writes.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive production change.
The slice adds one test selector, one CTest entry, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks` and `php-embedded-prod`.
- Run direct selector `random-savepoint-same-table-schedule`.
- Run the adjacent savepoint CTest subset.
- Run ownerless transaction/random-transaction stress smoke.
- Run production-build guard, format check, and `git diff --check`.

## Acceptance Criteria

- The overlapping schedule passes without exhausted retries.
- Final ownerless reopen, ordinary native reopen, forced `.shm` rebuild, and
  final native reopen all match the deterministic expected row count, value
  sum, version sum, and weighted value sum.
- No-live recovery clears native file-operation markers and drains
  checkpointable WAL in non-hook builds.

## Risks And Follow-Up

- This narrows randomized same-table savepoint coverage but does not close
  native mid-rollback crash injection.
- Longer external MariaDB/RQG-style schedules remain planned after the bounded
  ownerless recovery gates stop producing new correctness issues.
