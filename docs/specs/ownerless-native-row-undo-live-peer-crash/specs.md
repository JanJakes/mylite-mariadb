# Ownerless Native Row Undo Live-Peer Crash

## Problem Statement

Native row-undo rollback crash coverage kills full-transaction rollback and
`ROLLBACK TO SAVEPOINT` writers after one successful InnoDB `row_undo()` step,
then verifies no-live ownerless recovery. The remaining live-peer boundary is
stricter: while another ownerless process still has the directory open, a fresh
ownerless opener must not perform no-live cleanup of the killed writer's stale
rollback state. Once the peer exits, no-live recovery must still let
MariaDB/InnoDB finish the uncommitted rollback and preserve the original rows.
The FK/trigger follow-up broadens that same native row-undo crash boundary
from update-side effects to delete-side effects.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0roll.cc`
  - `trx_t::rollback_low()` builds and runs the rollback graph, then performs
    full-rollback or savepoint cleanup after the graph completes.
- `mariadb/storage/innobase/row/row0undo.cc`
  - `row_undo()` fetches one undo record, applies `row_undo_ins()` or
    `row_undo_mod()`, releases the undo page, closes the persistent cursor,
    clears the heap, and then fires the existing unsafe
    `rollback-after-native-row-undo` test fault when enabled.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - `savepoint-rollback-native-row-undo-crash` and
    `transaction-rollback-native-row-undo-crash` already verify no-live
    recovery after the same native row-undo fault.
  - `savepoint-rollback-prewrite-live-peer-crash` already proves the expected
    live-peer ownerless lifecycle for a post-native rollback boundary: a fresh
    ownerless opener returns `MYLITE_BUSY` while the peer remains live, then
    no-live recovery succeeds after peer release.
  - Existing FK/trigger native row-undo coverage exercises `ON UPDATE CASCADE`
    plus `AFTER UPDATE` trigger audit rows at a deterministic later rollback
    boundary. Direct local reproduction and adjacent sequence coverage of
    update-side skip values through `8` drain native recovered rollback but can
    leave parent or cascade child state partially applied; the registered
    selector therefore uses skip `9`, and
    earlier FK/trigger undo hits remain open work. Delete-side FK actions and
    delete trigger row images need separate evidence.

## Design

Add two hook-only selectors:

- `savepoint-rollback-native-row-undo-live-peer-crash`
- `transaction-rollback-native-row-undo-live-peer-crash`

Each selector reuses the existing native row-undo writer fault function and
data oracle, but holds an independent ownerless peer open before starting the
writer. After the writer reaches `rollback-after-native-row-undo`, the parent
kills it, proves a fresh ownerless read/write opener returns `MYLITE_BUSY`
while the peer remains live, releases the peer, verifies shared read-only
attachment is still rejected until read/write recovery runs, and then performs
the same no-live recovery, forced `.shm` rebuild, ordinary native reopen, and
follow-up write checks as the existing no-live selectors.

The FK/trigger update-side and delete-side follow-ups use deterministic later
rollback boundaries. The update-side selector reaches its proven boundary after
skipping nine native `rollback-after-native-row-undo` hits and covers
`ON UPDATE CASCADE` child-row changes plus `AFTER UPDATE` trigger audit rows.
The FK/trigger delete-side follow-up adds one ownerless SQL hook case:

- `test_crashed_fk_trigger_delete_native_row_undo_with_live_peer_blocks_recovery`

It deletes a parent row with one `ON DELETE CASCADE` child table and one
`ON DELETE SET NULL` child table, deletes two trigger base rows with an
`AFTER DELETE` audit trigger, then kills the rollback writer at
`rollback-after-native-row-undo` after skipping five native `row_undo()` hits.
Those selectors target deterministic later rollback boundaries for FK/trigger
side effects rather than claiming every native FK/trigger undo substep is safe.
A fresh ownerless opener must remain busy while the peer is live; after peer
release, no-live recovery must restore the deleted parent, cascade child row,
set-null child key, trigger base rows, and remove the rolled-back audit rows.

No production code changes are required.

## Scope And Non-Goals

In scope:

- Linux unsafe ownerless test-hook coverage.
- One native row-undo crash point for full rollback and one for savepoint
  rollback.
- Live-peer busy behavior before no-live recovery.
- Delete-side FK `CASCADE`/`SET NULL` actions and `AFTER DELETE` trigger audit
  rows at the same native row-undo fault in the live-peer busy/recovery path.
- Ownerless read/write recovery after peer release, forced `.shm` rebuild,
  ordinary native reopen, and follow-up native writes.

Out of scope:

- Exhaustive faults inside every `row_undo_ins()` or `row_undo_mod()` substep.
- FK actions beyond the covered update-cascade and delete cascade/set-null
  shapes, trigger variants beyond the covered update/delete audit rows,
  generated-column side effects beyond the existing focused case, DDL rollback,
  XA rollback, prepared transactions, or earlier native FK/trigger row-undo
  hits before the update-side skip-9 and delete-side skip-5 boundaries.
- Longer randomized savepoint schedules and external MariaDB/RQG stress.
- SQL-level table-lock fault injection.

## Compatibility Impact

No SQL syntax, C API, storage-format, or production behavior changes. The slice
adds evidence that a killed writer inside native rollback internals does not
let another opener perform no-live ownerless cleanup while a peer is still
live, and that final no-live recovery still preserves MariaDB/InnoDB rollback
semantics. The FK/trigger follow-up adds compatibility evidence for delete-side
referential actions and trigger side effects under the same rollback crash
boundary.

## Directory, Lifecycle, And Native Storage Impact

No durable directory-layout or native storage-format change. The tests keep all
database files and ownerless runtime files inside the MyLite database directory,
retain live-peer ambiguity until the peer exits, then prove recovery,
checkpointing or native-support-only retained evidence, forced shared-memory
rebuild, and ordinary native reopen all observe the same original rows.

## Public API, Build, Size, And License

No public API, dependency, license, or binary-size-sensitive profile change.
The selectors are registered only for the unsafe ownerless hook build.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the direct live-peer native row-undo selectors.
- Run the delete-side FK/trigger native row-undo SQL cases by name.
- Run the adjacent registered rollback hook CTest subset.
- Build the production embedded target and run adjacent production
  transaction/savepoint smoke selectors.
- Run production CI-build guards, format checks, and `git diff --check`.

## Acceptance Criteria

- The writer reaches the existing `rollback-after-native-row-undo` fault after
  at least one native row undo succeeds.
- The FK/trigger update-side case reaches the same fault after nine skipped
  native row-undo hits, and the delete-side case reaches it after five skipped
  native row-undo hits, after FK and trigger side-effect undo records have made
  progress.
- A fresh ownerless read/write opener returns `MYLITE_BUSY` while another
  ownerless peer remains live.
- After peer release, shared read-only attachment remains busy until read/write
  recovery runs.
- No-live ownerless recovery preserves the original row values and payloads,
  keeps native DML/file-operation markers clear, checkpoints ownerless WAL or
  retains only native-support rollback-history evidence, survives forced
  `.shm` rebuild, and remains writable through ordinary native reopen.
- Delete-side FK/trigger recovery restores the parent rows, cascade child rows,
  set-null child keys, trigger base rows, and removes rolled-back audit rows
  after live-peer release and no-live recovery.

## Risks And Follow-Up

- This covers a deterministic live-peer lifecycle around the existing row-undo
  fault, not every native undo sub-operation.
- Additional FK/trigger/generated-column rollback side-effect matrices,
  XA/prepared rollback, longer randomized savepoint schedules, broader
  redo/checkpoint reconciliation, and external MariaDB/RQG stress remain
  completion work.
