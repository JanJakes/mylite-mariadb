# Ownerless InnoDB External Waits

## Problem

The ownerless InnoDB lock hooks mirror granted native locks into
`concurrency/mylite-concurrency.shm`, but a process can still grant an InnoDB
table or record lock locally before checking whether another process already
holds a conflicting lock. That is only diagnostic. Full ownerless write
concurrency needs the directory-owned lock registry to participate in the grant
decision, wait/wake protocol, and deadlock graph.

This slice turns the InnoDB lock registry from a granted-lock mirror into the
authoritative cross-process wait surface for InnoDB table and record locks. It
still must remain behind the ownerless product gate until page visibility,
DDL dictionary invalidation, and crash recovery are complete.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/lock/lock0lock.cc` owns InnoDB lock grant, wait,
  wake, cancellation, and deadlock behavior through process-local `lock_sys`.
- `lock_rec_lock()` checks process-local explicit record locks under
  `lock_sys.rec_hash`, then either enqueues a waiting request with
  `lock_rec_enqueue_waiting()` or grants immediately through
  `lock_rec_add_to_queue()`, `lock_rec_create()`, and `lock_rec_set_nth_bit()`.
- `lock_table_low()` checks process-local table locks through
  `lock_table_other_has_incompatible()`, then either enqueues through
  `lock_table_enqueue_waiting()` or grants through `lock_table_create()`.
- `lock_grant()` converts a waiting `lock_t` into a granted lock, clears
  `trx->lock.wait_lock`, signals `trx->lock.cond`, and is reached from record
  and table dequeue paths while local lock-system latches are held.
- `lock_wait()` sleeps on `trx->lock.cond` under `lock_sys.wait_mutex`, runs the
  process-local deadlock detector, handles timeouts and interruptions, and
  cancels waiting locks on timeout or victim selection.
- `Deadlock::find_cycle()` and `Deadlock::report()` traverse
  `trx->lock.wait_trx` pointers. They cannot see a transaction in another
  process and cannot see an external wait unless MyLite publishes a parallel
  wait edge.
- `lock_rec_enqueue_waiting()` and `lock_table_enqueue_waiting()` create local
  waiting locks for process-local conflicts. A mixed cross-process deadlock can
  include those local wait edges, so MyLite must publish local wait edges to
  the directory-owned graph too.
- The current ownerless registry stores granted lock entries only. Its blocking
  acquire path waits on the conflicting granted slot's wait word, but it does
  not store a stable waiter record, queue ordering, deadlock edges, or victim
  state.

## Design

### Shared Registry

Extend `ownerless_innodb_lock_registry` with waiting entries:

- active granted entries keep the existing fields,
- waiting entries store the requested table/record key, mode, flags,
  waiter owner slot, waiter slot generation, waiter transaction ID, blocker
  owner slot, blocker transaction ID, blocker slot generation, wait generation,
  timeout/victim state, and a wake word,
- same owner slot and same transaction ID remains reentrant,
- the same numeric transaction ID from another owner slot is always distinct,
- normal owner cleanup removes both granted entries and waiting entries for the
  owner being closed, then wakes waiters that may have been blocked by them.
  Dead-owner cleanup preserves granted or waiting entries while any live peer
  remains, because those entries are transaction-recovery evidence until a
  durable recovery path can decide whether the dead transaction committed or
  rolled back.

The registry must publish an explicit wait edge before a process sleeps on an
external conflict. Before sleeping, it traverses waiting entries by
`(owner, trx_id)` to detect a cycle. If the new edge closes a cycle, the caller
receives a deadlock result instead of waiting for timeout.

Waiting entries are directory-owned coordination, not durable truth. They are
discarded with the rest of `.shm` during recovery rebuild. Normal close removes
the owner's waiting entries. Dead-owner waiting or granted entries are preserved
while live peers remain, then discarded by a no-live-process recovery rebuild or
by a future durable recovery path that proves the corresponding transaction can
no longer own the lock.

### InnoDB Grant Boundary

The registry must be consulted before a lock becomes globally granted:

1. If a local process already holds an equal or stronger lock, InnoDB may return
   success without touching the registry.
2. If a local conflict exists, InnoDB enqueues the normal local waiting lock and
   MyLite publishes a directory wait edge from the waiting transaction to the
   local blocker. This makes mixed local/external deadlocks visible.
3. If no local conflict exists, MyLite tries to reserve the requested lock in
   the shared registry before creating or granting the native `lock_t`.
4. If the shared registry reports an external conflict, MyLite publishes an
   external wait edge, releases local latches, sleeps on the mapped wait word,
   then retries the native lock path from the beginning.
5. If a local waiting lock becomes locally grantable, the grant path must obtain
   the shared registry entry before clearing `LOCK_WAIT` and waking the SQL
   thread. If the shared registry still conflicts, the local waiting lock
   remains waiting and the SQL thread is woken only to wait on the external
   wait edge or to observe timeout, interruption, or deadlock.

Long external waits must never happen while holding `lock_sys` latches,
`table->lock_mutex`, `lock_sys.wait_mutex`, `trx->mutex`, or a page latch. Only
short nonblocking registry checks are allowed under local InnoDB latches.

### Wait/Wake Bridge

InnoDB waits are process-local, while MyLite external wait words are
directory-owned. The bridge therefore needs two states:

- `trx->lock.wait_lock` remains the local SQL wait anchor,
- a MyLite external wait state records the directory waiter and blocker IDs.

When a local waiter is blocked only by an external registry conflict, the grant
path leaves `LOCK_WAIT` set, records the external wait, and signals
`trx->lock.cond`. The waiting thread releases `lock_sys.wait_mutex`, sleeps on
the mapped wait word, then asks the local grant path to retry. This prevents a
release thread from blocking on another process while holding local latches.

Timeouts, interrupts, and local deadlock victims must cancel both the local
waiting lock and the directory wait entry. External deadlock victims must be
reported as `DB_DEADLOCK`.

### Deadlock Policy

MyLite uses the shared registry to detect cycles that cross process
boundaries. The first complete implementation may choose the current requester
as the victim when adding the edge would close a cross-process cycle; that is
safe and deterministic. A later refinement can mirror MariaDB's detailed victim
weighting when enough cross-process transaction weight and nontransactional
table-edit evidence are available.

Process-local cycles remain handled by MariaDB's existing detector. Mixed
cycles are visible because local waits publish wait edges to the shared
registry as soon as `lock_rec_enqueue_waiting()` or
`lock_table_enqueue_waiting()` succeeds.

## Scope

- Add waiting entries, deadlock detection, and cancellation to the ownerless
  InnoDB lock registry primitive.
- Extend the InnoDB hook surface so native code can:
  - preflight/try-acquire before a local grant,
  - publish a local wait edge,
  - publish an external wait edge,
  - clear a wait edge on grant, timeout, cancel, rollback, or close,
  - translate registry timeout/deadlock results to InnoDB error codes.
- Integrate table-lock and record-lock paths enough for deterministic
  two-process lock wait and deadlock tests behind the ownerless test gate.
- Keep public ownerless read/write capability disabled.

## Non-Goals

- Do not enable `MYLITE_CAP_OWNERLESS_RW` in the public product path.
- Do not remove `mylite.lock` from normal opens in this slice.
- Do not solve page visibility, redo/LSN allocation, checkpointing, purge
  ownership, DDL dictionary invalidation, or crash recovery.
- Do not support spatial predicate/page predicate locks yet.
- Do not support MyISAM, Aria, or MEMORY in ownerless write mode.

## Compatibility Impact

The target behavior is MariaDB-compatible InnoDB lock semantics for
cross-process conflicts: compatible non-conflicting writers proceed, conflicting
writers wait, lock wait timeout maps to the normal InnoDB timeout error, and
deadlocks map to `DB_DEADLOCK`. Public product claims remain unchanged until
the later page-visibility and recovery slices pass.

## Directory And Lifecycle Impact

The `.shm` InnoDB lock-registry segment format changes because it now stores
waiting entries and wait graph metadata. The `.shm` format version must
increase. Existing `.shm` files are volatile and can be rebuilt under
`RECOVERY`.

No durable file format changes are made in this slice.

## Native Storage Impact

InnoDB native table and record lock objects remain process-local caches. The
directory-owned registry is the cross-process authority for conflicts and wait
edges, but InnoDB still owns local row access, rollback, undo, and lock object
memory.

## Implementation Status

The current branch implements the shared wait-entry primitive, local InnoDB
wait-edge publication, idempotent lock reservations, nonblocking pre-grant
reservation for native InnoDB table and record lock grants, the external
wait/retry bridge, and local waiting-lock grant deferral when a shared-registry
conflict remains. The long wait path snapshots the native table or record wait
key while the InnoDB lock system is protected, then sleeps only on the mapped
registry wait word after local latches are released. Embedded SQL coverage now
verifies that a conflicting external record entry makes a native InnoDB update
publish a shared wait, wake and complete after the external holder releases,
and clear both local and directory wait state after a normal InnoDB lock-wait
timeout.

This slice originally stopped before product ownerless read/write enablement.
Later ownerless slices moved cross-process SQL writer coverage into normal
embedded builds. Write commits flush dirty pages through the committing
transaction's LSN before releasing shared locks. External record waits advance
the local redo boundary and refresh only the waited record page before retrying
the local grant, because a global buffer-pool refresh inside an active writer
transaction can discard that process's own uncommitted dirty pages. Because
each process still owns a separate InnoDB buffer pool, cross-process X record
locks on different heap records of the same physical page are conservatively
treated as conflicting in the shared registry. That page-aware rule prevents
whole-page image flushes from overwriting a peer process's same-page row change
while allowing independent pages to proceed concurrently. Ownerless embedded
waits use the current SQL thread's session lock-wait timeout when the InnoDB
transaction does not carry `trx->mysql_thd`, so external waits keep the same
timeout semantics as SQL row-lock waits.

## Test Plan

- Registry primitive tests:
  - a waiter publishes and clears a wait edge,
  - release wakes a waiting entry,
  - timeout clears the wait edge,
  - normal owner cleanup removes granted and waiting entries,
  - dead-owner cleanup can block process-slot cleanup when recovery-sensitive
    state remains,
  - two-process deadlock cycle returns the new deadlock result,
  - same numeric transaction ID in another owner slot is still distinct.
- InnoDB hook tests:
  - local wait enqueue publishes a shared wait edge,
  - wait cancel clears the shared wait edge,
  - waiting-lock grant first obtains the registry entry,
  - external registry timeout maps to lock wait timeout,
  - external deadlock maps to `DB_DEADLOCK`.
- SQL tests behind ownerless test hooks:
  - two processes update different rows successfully, with same-page rows
    serialized by the shared registry's physical page rule,
  - two processes update the same row and one waits until commit,
  - two processes update rows in separate tables in reverse order and one
    receives a deadlock,
  - several processes mix ownerless reads and same-page writes without losing
    committed updates,
  - a timed wait returns the expected SQL lock wait timeout,
  - closing or killing a waiting process leaves no wait entry.
- Keep existing embedded, ownerless primitive, MDL, transaction, read-view, and
  lock-hook tests passing.

## Acceptance Criteria

- The shared registry contains both granted locks and wait edges.
- A process cannot globally grant a conflicting InnoDB table or record lock
  while another process holds an incompatible granted entry.
- X record locks on different records of the same InnoDB page serialize across
  processes until the shared buffer-pool or redo-replay design can prove safe
  page-image merging.
- Cross-process lock wait wakeups do not rely on process-local condition
  variables alone.
- Cross-process and mixed local/external deadlock cycles are detected before
  lock wait timeout.
- Timeout, interruption, deadlock victim selection, commit, rollback, and close
  clean both local and directory wait state.
- No code path blocks on a mapped wait word while holding InnoDB local latches.
- Normal embedded builds route cross-process SQL writer tests through
  `MYLITE_OPEN_OWNERLESS_RW` instead of the raw environment bypass. The unsafe
  preset remains only for deterministic fault injection and negative-proof
  coverage.

## Risks And Follow-Up

- Later ownerless slices added committed page visibility; DDL dictionary
  invalidation and broader stress coverage remain follow-up work.
- Registry capacity remains fixed. Product ownerless mode needs grow-only
  segment expansion before broad workloads.
- MariaDB's full deadlock victim weighting cannot be exactly reproduced until
  more transaction weight and nontransactional-table state is shared.
- Spatial predicate locks remain unsupported and must gate ownerless mode for
  spatial index workloads until designed.
