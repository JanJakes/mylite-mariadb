# Ownerless Same-Row Savepoint Handoff

## Problem Statement

Ownerless transaction coverage proves independent-table savepoint handoff,
focused same-page serialization, and same-table large-row waiting. The
remaining transaction matrix still includes same-row conflicts where a writer
rolls back post-savepoint DML, keeps its pre-savepoint row update open, and a
peer tries to update that same row before the first writer commits.

This slice covers that same-row conflict. Writer A updates row 1, creates a
savepoint, updates row 2, rolls back to the savepoint, and stays open. Writer B
updates row 1 with `value = value + 1`. B must wait until A commits, then apply
to A's committed row so the final value proves native row-lock handoff and
ownerless page-write publication agree.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/transaction.cc:711-748` implements
  `trans_rollback_to_savepoint()` and delegates engine rollback to
  `ha_rollback_to_savepoint()`.
- `mariadb/sql/handler.cc:3382-3440` calls each participating engine's
  savepoint rollback callback.
- `mariadb/storage/innobase/handler/ha_innodb.cc:2920-2933` records the InnoDB
  undo number for a savepoint, and
  `mariadb/storage/innobase/handler/ha_innodb.cc:5026-5069` rolls back to the
  saved undo number.
- `mariadb/storage/innobase/lock/lock0lock.cc:748-872` implements record-lock
  wait checks such as `lock_rec_has_to_wait()`, which MyLite exposes through
  the ownerless lock registry and wait counters.

## Scope And Non-Goals

In scope:

- A Linux ownerless SQL selector for same-row savepoint handoff.
- Writer B waiting while writer A keeps row 1 locked after rollback to
  savepoint.
- Final state proving B updated A's committed row, while A's rolled-back row 2
  update is discarded.
- Ownerless reopen, forced `.shm` rebuild, and ordinary native reopen checks.

Out of scope:

- Crashes inside native InnoDB rollback internals.
- Exhaustive same-row deadlock or timeout matrices.
- Randomized same-table savepoint schedules and external MariaDB/RQG stress.

## Design

Add `concurrent-savepoint-same-row-handoff` to
`mylite_ownerless_cross_process_sql_test`.

The table has three rows. Writer A:

1. starts a transaction,
2. updates row 1 to value `11`,
3. creates a savepoint,
4. updates row 2 to value `21`,
5. rolls back to the savepoint, and
6. waits before commit.

Writer B starts while A is open and runs:

```sql
UPDATE app.ownerless_concurrent_savepoint_same_row
SET value = value + 1, payload = REPEAT('d', 256)
WHERE id = 1
```

The parent waits for ownerless/native wait evidence, verifies a peer still sees
the original committed rows while A is open and B is blocked, releases A, then
verifies final state: row 1 is `12`, row 2 is the original `20`, and row 1's
payload is B's `d` payload.

## Compatibility Impact

No SQL feature or API behavior changes. This is compatibility evidence for
MariaDB current-update row-lock behavior across ownerless processes after
savepoint rollback.

## Directory, Lifecycle, And Native Storage Impact

No directory layout or native format changes. The test exercises existing
ownerless write ownership, native row-lock waits, DML native file-operation
markers, page-version WAL retention, forced shared-memory rebuild, and ordinary
native reopen.

## Public API, Build, Size, License

No public API, dependency, license, or binary-size-sensitive production change.
The slice adds one test selector, one CTest entry, and documentation.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`
  and `php-embedded-prod`.
- Run direct selector `concurrent-savepoint-same-row-handoff`.
- Run the adjacent savepoint handoff CTest subset.
- Run transaction stress smoke, production-build guard, format check, and
  `git diff --check`.

## Acceptance Criteria

- Writer B waits while writer A keeps row 1 locked after rollback to savepoint.
- While writer A remains open, a peer sees only the original committed rows.
- Final ownerless reopen, forced `.shm` rebuild, and ordinary native reopen
  preserve row 1 as B's update on top of A's committed row and preserve row 2's
  original value.

## Risks And Follow-Up

- This is a bounded same-row update conflict, not proof of every same-row
  deadlock, timeout, or native rollback crash window.
- Native mid-rollback crash injection, randomized same-table savepoint
  schedules, and external MariaDB/RQG stress remain ownerless concurrency
  completion work.
