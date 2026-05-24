# Ownerless InnoDB Transaction Hooks

## Problem

Ownerless cross-process writers need InnoDB transaction identity and MVCC
read-view construction to use directory-owned coordination instead of one
process-local `trx_sys_t`. The existing MyLite transaction registry proves a
file-backed `MAP_SHARED` primitive, but InnoDB still allocates transaction IDs,
tracks active read-write transactions, assigns transaction serialisation
numbers, and snapshots read views through process-local state.

This slice adds a guarded MyLite hook surface at the InnoDB transaction-system
boundary and proves that ordinary SQL reaches it. A follow-up product-binding
slice registers the directory-owned transaction registry for normal persistent
opens. Neither slice enables product ownerless read/write opens.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/trx0sys.h`:
  - `trx_sys_t::get_max_trx_id()` feeds read-view reuse checks such as
    `ReadView::open()`, so a hook path must not leave it tied to stale
    process-local `m_max_trx_id`.
  - `trx_sys_t::get_new_trx_id()` allocates standalone transaction IDs from
    process-local `m_max_trx_id` and then refreshes
    `m_rw_trx_hash_version`.
  - `trx_sys_t::register_rw()` allocates the read-write transaction ID, inserts
    the transaction into process-local `rw_trx_hash`, and refreshes
    `m_rw_trx_hash_version`.
  - `trx_sys_t::assign_new_trx_no()` allocates a transaction serialisation
    number, stores it in `trx->rw_trx_hash_element->no`, and refreshes
    `m_rw_trx_hash_version`.
  - `trx_sys_t::snapshot_ids()` waits for `m_rw_trx_hash_version` to catch up
    to `m_max_trx_id`, copies active transaction IDs from process-local
    `rw_trx_hash`, and computes `min_trx_no` for purge safety.
  - `trx_sys_t::deregister_rw()` removes a read-write transaction from
    process-local `rw_trx_hash`.
- `mariadb/storage/innobase/read/read0read.cc`:
  - `ReadViewBase::snapshot()` calls `trx_sys.snapshot_ids()` and then sorts
    and compresses the active transaction ID set.
- `mariadb/storage/innobase/include/read0types.h`:
  - `ReadViewBase` uses `m_low_limit_id`, `m_up_limit_id`, `m_ids`, and
    `m_low_limit_no` for visibility and purge-limit decisions.
- `mariadb/storage/innobase/trx/trx0trx.cc`:
  - Read-write transaction assignment paths call `trx_sys.register_rw()`.
  - Commit paths call `trx_sys.assign_new_trx_no()` before cleanup.
  - Commit cleanup calls `trx_sys.deregister_rw()`.
- `mariadb/storage/innobase/handler/ha_innodb.cc`:
  - Versioning paths can call `trx_sys.get_new_trx_id()` independently of
    active read-write transaction registration.

## Design

Add a first-party InnoDB hook surface:

- `mylite_ownerless_trx_allocate()` for standalone transaction ID allocation.
- `mylite_ownerless_trx_register()` for read-write transaction ID allocation
  and active transaction publication.
- `mylite_ownerless_trx_assign_no()` for atomic transaction
  serialisation-number allocation and publication.
- `mylite_ownerless_trx_deregister()` for active transaction removal.
- `mylite_ownerless_trx_snapshot()` for active transaction ID snapshots,
  next-ID reporting, oldest serialisation-number reporting, and
  `trx_sys_t::get_max_trx_id()` when hooks are installed.

Hooks are optional. When no complete hook set is installed, InnoDB follows the
unchanged MariaDB path. When a hook is installed and returns an error other
than "unavailable", InnoDB aborts instead of silently mixing ownerless and
process-local transaction identities.

The hook surface is intentionally lower than SQL and higher than the shared
registry primitive. Product persistent opens now bind the hook to
`concurrency/mylite-concurrency.shm`; the binding is still guarded by the
exclusive directory lock because the remaining ownerless writer surfaces are
not complete.

## Compatibility Impact

No user-visible SQL behavior changes. With no hooks installed, the transaction
path is expected to be behavior-preserving. Tests install hooks only inside an
embedded test process and verify that ordinary InnoDB SQL reaches the hook
surface.

Ownerless read/write remains unavailable. This slice is evidence for the next
transaction-visibility phase, not product support.

## Directory And Lifecycle Impact

No directory format changes. The production `.shm` transaction registry is now
registered with InnoDB for normal persistent opens, after seeding the registry
next-ID floor from recovered InnoDB transaction-system state.

## Native Storage Impact

InnoDB remains the native storage engine. This slice adds a narrow fork delta
around InnoDB's transaction-system boundary. It does not change redo, undo,
record locks, buffer-pool visibility, dictionary state, or purge behavior.

## Test Plan

- Add an embedded InnoDB SQL test that installs transaction hooks and executes
  DDL, DML, explicit transactions, reads, update, rollback, and commit.
- Assert that register, assign serialisation number, snapshot, and deregister
  callbacks are reached and balanced.
- Assert that hook reset restores the no-hook state.
- Run focused ownerless hook and primitive tests.
- Run the full `embedded-dev` test suite.

## Acceptance Criteria

- The hook surface builds into the embedded InnoDB target.
- Ordinary InnoDB SQL reaches the hook surface under test.
- Existing no-hook tests keep passing.
- Documentation continues to mark ownerless read/write as planned, not
  supported.

## Risks And Unresolved Questions

- Correct cross-process MVCC still requires all processes to use the
  directory-owned transaction registry together with shared record locks, page
  visibility, purge, redo, checkpoint, and recovery coordination.
- Correct cross-process writers also need shared or otherwise coherent InnoDB
  record locks, buffer-pool/page visibility, redo append ordering, checkpoint
  coordination, purge ownership, and crash recovery.
- The hook surface aborts on ownerless hook errors because continuing with
  process-local transaction IDs after partial ownerless publication would be
  unsafe.
