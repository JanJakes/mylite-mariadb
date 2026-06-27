# Ownerless Concurrent Savepoint Rollback Handoff

## Problem Statement

Ownerless savepoint coverage proves single-writer marker classification,
killed writers before and after `ROLLBACK TO SAVEPOINT`, and the MyLite-owned
post-native rollback state boundary. The remaining documented gap is a
concurrent-writer savepoint schedule: one ownerless writer rolls back
post-savepoint DML while another ownerless writer commits independent DML
through the same database directory lifecycle.

The risk is that MyLite's handle-local savepoint state or native file-operation
evidence could incorrectly publish the rolled-back image, discard a peer's
committed image, or drain retained ownerless WAL while another writer is still
live.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/transaction.cc:711` implements
  `trans_rollback_to_savepoint()` by finding the SQL savepoint and delegating
  native rollback to `ha_rollback_to_savepoint()` before releasing eligible
  metadata locks.
- `mariadb/sql/handler.cc:3382` implements
  `ha_rollback_to_savepoint()` by calling each participating storage engine's
  `savepoint_rollback` callback for engines active at the savepoint and full
  rollback for engines registered after it.
- `mariadb/storage/innobase/handler/ha_innodb.cc:2920` stores InnoDB's current
  undo number in `innobase_savepoint()`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:5033` implements
  `innobase_rollback_to_savepoint()` by rolling the transaction back to the
  saved undo number, refreshing FTS savepoint state, and updating
  `last_stmt_start`.
- `packages/libmylite/src/database.cc:18688` updates MyLite's process-local
  ownerless savepoint stack only after SQL succeeds. Its `ROLLBACK` branch
  restores the local-write flag from the target savepoint and discards native
  file-operation redo only for writes that no longer survive the savepoint
  boundary.
- `packages/libmylite/src/database.cc:18889` clears transaction-local
  savepoint and write state after transaction-ending SQL.

## Scope And Non-Goals

In scope:

- Add a deterministic Linux SQL test where writer A keeps an explicit
  ownerless transaction open after rolling back post-savepoint DML, writer B
  commits independent explicit DML while A remains live, then A commits only
  its pre-savepoint DML.
- Prove B's committed image is visible while A's uncommitted image remains
  hidden.
- Prove final ownerless reopen, forced `.shm` rebuild, and ordinary native
  reopen preserve exactly A's pre-savepoint image and B's committed image.
- Prove live-peer native DML file-operation marker retention/drain remains
  consistent across the schedule.

Out of scope:

- Crash injection inside InnoDB's native savepoint rollback internals.
- Same-page or same-row physical contention while the savepoint transaction is
  open.
- Broad randomized concurrent-writer savepoint matrices.
- DDL/file-lifecycle recovery, active-reader pressure breadth, or external
  MariaDB/RQG stress expansion.

## Design

The focused test creates two independent file-per-table InnoDB tables. Writer A
opens ownerless read/write, starts an explicit transaction, updates table A
before a savepoint, updates another row in table A after the savepoint, rolls
back to the savepoint, verifies the pre-savepoint row survives locally and the
post-savepoint row is gone, then waits before committing.

While writer A is waiting, the parent process opens a separate ownerless
read/write handle, starts and commits an explicit transaction against table B,
and verifies table B's committed row is visible while table A still shows only
pre-test committed rows. Because writer A is still live, the test expects the
native DML file-operation marker/WAL to remain retained instead of being drained
by writer B's close.

After writer A is released and closes, the test verifies final ownerless state,
forced shared-memory rebuild state, ordinary native reopen state, and a
follow-up native write. No product code change is expected unless this schedule
exposes a real ownerless recovery bug.

## Compatibility Impact

SQL semantics remain MariaDB-owned. The slice adds compatibility evidence that
supported MariaDB savepoint rollback and explicit transaction commit semantics
remain observable when separate embedded ownerless processes write
concurrently.

## Directory And Lifecycle Impact

All durable state remains in the MyLite-owned database directory. The test
checks live-peer marker retention, final no-live marker drain, ownerless reopen,
forced `.shm` rebuild, and ordinary native reopen.

## Native Storage Impact

Native InnoDB undo remains authoritative for the rolled-back post-savepoint row.
The peer writer uses a separate table so this slice targets ownerless
transaction evidence handoff and directory lifecycle, not InnoDB row or page
lock contention.

## Public API, Build, Size, License

No public API, dependency, license, or binary-profile changes. The slice adds
one focused ownerless SQL selector, one CTest entry, and compatibility/spec
documentation.

## Test And Verification Plan

- Add selector `concurrent-savepoint-rollback-handoff`.
- Register production CTest
  `libmylite.ownerless-concurrent-savepoint-rollback-handoff`.
- Run the new selector directly in production and hook builds.
- Run adjacent savepoint marker selectors:
  `savepoint`,
  `native-explicit-dml-savepoint-file-op-marker-discard`,
  `native-explicit-dml-savepoint-file-op-marker-retain`,
  `native-multi-peer-explicit-dml-file-op-marker-drain`, and
  `native-killed-before-savepoint-rollback-dml-file-op-marker-recovery`.
- Run an ownerless stress smoke that includes transaction/savepoint stress.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Writer B can commit explicit DML while writer A holds an ownerless explicit
  transaction whose post-savepoint write has been rolled back.
- During writer A's open transaction, a separate ownerless opener sees B's
  committed row and does not see A's uncommitted or rolled-back rows.
- After writer A commits, ownerless reopen, forced `.shm` rebuild, and ordinary
  native reopen preserve exactly A's pre-savepoint row plus B's row.
- The native DML marker is retained while a peer is live and drained after the
  final no-live close.
- Adjacent savepoint and explicit DML marker selectors continue to pass.

## Verification Results

- Production build selector:
  `concurrent-savepoint-rollback-handoff`.
- Production focused CTest:
  `libmylite.ownerless-concurrent-savepoint-rollback-handoff`,
  `libmylite.ownerless-explicit-dml-savepoint-marker-discard`,
  `libmylite.ownerless-explicit-dml-savepoint-marker-retain`, and
  `libmylite.ownerless-killed-savepoint-dml-marker-recovery`.
- Production adjacent direct selectors:
  `savepoint`,
  `native-killed-before-savepoint-rollback-dml-file-op-marker-recovery`, and
  `native-multi-peer-explicit-dml-file-op-marker-drain`.
- Hook build CTest:
  `libmylite.ownerless-concurrent-savepoint-rollback-handoff`.
- Stress preset:
  `libmylite.ownerless-cross-process-transaction-stress` and
  `libmylite.ownerless-cross-process-random-transaction-stress`.
- Guards:
  `tools/check-ci-production-builds`,
  `tools.ci-production-builds`, `format-check`, and `git diff --check`.
- GitHub Actions run `28289007697` for the previous pushed head
  `df40c6c38bf50876882ff3578c221412337a020d` completed successfully with
  36 green jobs and no failures.

## Risks And Follow-Up

- This is an independent-table concurrent-writer schedule. Same-page
  savepoint/write contention, crash injection inside native rollback internals,
  and broader randomized concurrent-writer savepoint matrices remain planned.
- Broader native redo/checkpoint reconciliation, arbitrary DDL file-lifecycle
  recovery, active-reader pressure crash/oracle breadth, and external
  MariaDB/RQG stress remain ownerless-concurrency completion work.
