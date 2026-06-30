# Ownerless FK Trigger Delete Row Undo No-Live Crash

## Problem Statement

The native row-undo live-peer slice covers a delete-side FK/trigger rollback
boundary only while another ownerless process remains live. The ownerless
completion list still calls out standalone delete-side no-live stabilization.
MyLite needs direct evidence that, when no peer is live, the next ownerless
read/write opener can recover a writer killed during native rollback of
`ON DELETE` referential actions and delete-trigger side effects.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0roll.cc` implements
  `trx_t::rollback_low()`, which builds the rollback graph, runs
  `que_run_threads(roll_node->undo_thr)`, then completes transaction or
  savepoint rollback cleanup after the graph finishes.
- `mariadb/storage/innobase/row/row0undo.cc` implements `row_undo()`. The
  existing unsafe `rollback-after-native-row-undo` test fault fires after a
  successful `row_undo_ins()` or `row_undo_mod()` operation, after the undo
  page is unfixed, the persistent cursor is closed, and the row-undo heap is
  cleared.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  the delete-side fixture and writer:
  `rollback_fk_trigger_delete_transaction_until_native_row_undo_fault()` deletes
  a parent row with `ON DELETE CASCADE` and `ON DELETE SET NULL` children,
  deletes trigger base rows with an `AFTER DELETE` audit trigger, then rolls
  back with `MYLITE_OWNERLESS_TEST_FAULT_SKIP=3` so the fault hits a later
  deterministic native row-undo step.
- The existing live-peer selector proves `MYLITE_BUSY` gating while a peer is
  live, then no-live recovery after peer release. It does not separately prove
  immediate no-live recovery when the killed writer is the last ownerless
  process.

## Design

Add one hook-only no-live selector:

- `transaction-rollback-fk-trigger-delete-row-undo-crash`

The selector reuses the existing delete-side fixture, fault writer, recovery
oracle, forced `.shm` rebuild, and follow-up native write checks. The shared
helper now takes a `hold_live_peer` flag, matching the update-side FK/trigger
row-undo helper:

- `hold_live_peer=0` kills the rollback writer, immediately opens ownerless
  read/write recovery, waits for MariaDB's recovered transaction rollback to
  drain, verifies original rows and clear native markers, forces `.shm`
  rebuild, and proves follow-up ordinary native FK/trigger writes.
- `hold_live_peer=1` preserves the existing live-peer behavior and registered
  selector.

No production code change is required.

## Scope And Non-Goals

In scope:

- Linux unsafe ownerless hook-build coverage.
- Standalone no-live recovery after the existing later delete-side native
  `row_undo()` fault.
- FK `ON DELETE CASCADE`, FK `ON DELETE SET NULL`, and `AFTER DELETE` trigger
  audit rows.
- Clear native DML/file-operation markers, recovered-native-transaction drain,
  ownerless WAL checkpoint or native-support-only retention, forced `.shm`
  rebuild, ordinary native reopen, and follow-up native writes.

Out of scope:

- Faults inside every `row_undo_ins()` or `row_undo_mod()` substep.
- Earlier FK-delete row-undo hits before the existing skip-3 deterministic
  point.
- Additional FK action shapes, trigger variants, XA/prepared rollback, DDL
  rollback, randomized fault selection, or external MariaDB/RQG stress.
- SQL-level table-lock fault injection.

## Compatibility Impact

No SQL syntax, public C API, storage format, or production runtime behavior
changes. The slice adds crash-recovery evidence for MariaDB-supported
delete-side referential actions and triggers under ownerless mode.

## Directory, Lifecycle, And Native Storage Impact

No durable directory-layout or native storage-format change. The test verifies
that all durable state remains in the MyLite-owned database directory, that
no-live ownerless recovery can let native InnoDB finish rollback, and that
partial delete-side row-undo state is not published as committed ownerless
page-version WAL.

## Public API, Build, Size, And License

No public API, dependency, license, or binary-size-sensitive profile change.
The new selector is registered only in the unsafe ownerless hook build.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused no-live and live-peer delete-side selectors:
  `libmylite.ownerless-transaction-rollback-fk-trigger-delete-row-undo-crash`
  and
  `libmylite.ownerless-transaction-rollback-fk-trigger-delete-row-undo-live-peer-crash`.
- Run the adjacent native-row-undo live-peer selector to prove
  native-support-only rollback-history retention remains accepted where the
  native evidence has not fully checkpointed.
- Build the production embedded target and run adjacent production transaction
  smoke selectors.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- The writer reaches `rollback-after-native-row-undo` after skipping three
  earlier native row-undo hits.
- No-live ownerless recovery waits for MariaDB's recovered transaction rollback
  to drain, restores the deleted parent row, cascade child row, set-null child
  key, trigger base rows, and removes rolled-back audit rows.
- Both native DML and generic file-operation markers remain clear.
- Ownerless WAL checkpoints or is reduced to native-support-only evidence after
  recovery.
- Forced `.shm` rebuild and ordinary native reopen observe the same recovered
  state.
- Follow-up ordinary native FK/trigger writes succeed.
- The existing live-peer delete-side selector still passes.

## Risks And Follow-Up

- The slice covers the stable later delete-side row-undo boundary, not every
  earlier FK-delete undo substep.
- Broader FK/trigger rollback crash matrices, XA/prepared rollback, longer
  randomized savepoint schedules, broader redo/checkpoint reconciliation,
  DDL/file lifecycle recovery, and external MariaDB/RQG stress remain planned.
