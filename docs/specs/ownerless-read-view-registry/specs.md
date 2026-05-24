# Ownerless Read View Registry

## Problem

The ownerless transaction registry makes active read-write transaction IDs
visible across mapped processes, but purge safety also depends on active read
views. MariaDB's purge coordinator clones local `ReadView` objects from
`trx_sys.trx_list`; a separate MyLite process with an old snapshot is currently
invisible to that clone. Cross-process writers cannot be correct until purge
oldest-view state is directory-owned too.

This slice adds a bounded directory-backed read-view registry in
`concurrency/mylite-concurrency.shm`, hooks InnoDB read-view open/close and
purge oldest-view cloning into it, and binds normal persistent product opens to
the registry. It still does not remove the exclusive directory lock or enable
ownerless read/write opens.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/read/read0read.cc` builds a `ReadView` by calling
  `ReadViewBase::snapshot()`, then makes it visible to purge with
  `ReadView::m_open`.
- `trx_sys_t::clone_oldest_view()` first snapshots current read-write
  transactions, then appends every locally open transaction read view from
  `trx_sys.trx_list`.
- `ReadViewBase::append()` defines the merge semantics required by purge:
  choose the minimum `low_limit_no`, choose the minimum `low_limit_id`, and keep
  active transaction IDs below the merged `low_limit_id`.
- `ReadView::close()` is the owner-thread point where the view becomes
  invisible to purge.
- `purge_sys_t::clone_oldest_view()` uses `trx_sys.clone_oldest_view(&view)` as
  the purge visibility boundary.

## Design

- Add `ownerless_read_view_registry` as a first-party shared-memory primitive.
  Each active slot stores:
  - owner process ID,
  - slot generation,
  - `low_limit_id`,
  - `low_limit_no`,
  - the read view's active transaction ID list.
- Keep slot storage fixed for this slice. The per-view ID capacity matches the
  current transaction-registry slot count, so a read view can publish every
  active transaction ID that the current shared transaction registry can expose.
  Exceeding that capacity fails closed.
- Add guarded InnoDB hook functions:
  - register a read view after snapshot creation and before `m_open` is set,
  - deregister it during `ReadView::close()`,
  - snapshot all remote read views during `trx_sys_t::clone_oldest_view()`.
- Add `ReadViewBase::append_ownerless()` so the purge clone can merge the
  directory-owned oldest view through the same semantics as local read views.
- Bind the hook context from persistent `mylite_open()` alongside the existing
  MDL and transaction hooks.
- Release read-view slots owned by dead or closing process slots during process
  cleanup.

## Compatibility Impact

No public capability changes. The exclusive directory lock remains in product
opens, and ownerless read/write capability remains disabled. The change
removes one correctness gap that would otherwise allow purge to ignore a read
view in another process.

## Directory And Lifecycle Impact

The rebuildable `.shm` layout gains a read-view-registry segment and bumps the
`.shm` format. The segment is not durable truth; it is reconstructed during
normal concurrency metadata rebuilds. Active read-view slots are cleaned during
close and dead process-slot cleanup.

## Native Storage Impact

InnoDB read views remain native `ReadView` objects. MyLite only mirrors the
purge-relevant read-view boundaries into directory-owned shared state.

## Test Plan

- Add primitive tests for:
  - read-view open/close,
  - oldest-view snapshot merging across mapped parent/child views,
  - insufficient output capacity,
  - per-view ID capacity fail-closed behavior,
  - owner cleanup.
- Extend embedded open/close coverage to prove normal persistent SQL opens a
  read view, publishes it to the production `.shm` segment, and removes it on
  commit/final close.
- Run focused ownerless primitive, transaction-hook, and open/close tests.
- Run the full `embedded-dev` test suite and ownerless negative proof.

## Acceptance Criteria

- Every normal persistent InnoDB read view is published before it becomes
  visible to local purge.
- Closing a read view removes its shared slot.
- Purge oldest-view cloning merges directory-owned read views.
- Registry failures fail closed instead of silently allowing unsafe purge.
- Existing `:memory:` behavior and product capability flags remain unchanged.

## Risks And Unresolved Questions

- Fixed per-view ID capacity is acceptable for the current bounded `.shm`
  profile but must become dynamically growable before ownerless read/write mode
  is enabled without concurrency limits.
- This slice does not solve record/table row-lock queues, redo append ordering,
  checkpoint visibility, buffer-pool/page invalidation, or crash recovery for
  in-flight writers.
