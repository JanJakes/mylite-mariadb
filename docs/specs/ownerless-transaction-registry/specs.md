# Ownerless Transaction Registry

## Problem

Ownerless write concurrency cannot rely on each embedded MariaDB process having
its own private InnoDB transaction system. Cross-process MVCC needs a
directory-owned source of transaction IDs and a visible set of active
read-write transactions before read views, purge limits, lock ownership, redo,
and commit publication can be made correct.

This slice adds the first MyLite-owned transaction-registry primitive and wires
it into the production `concurrency/mylite-concurrency.shm` layout. It does not
hook MariaDB/InnoDB transaction paths yet and does not enable
`MYLITE_CAP_OWNERLESS_RW`.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`.
- `mariadb/storage/innobase/include/trx0sys.h`:
  - `trx_sys_t::get_new_trx_id()` allocates from `m_max_trx_id` and refreshes
    `m_rw_trx_hash_version`.
  - `trx_sys_t::snapshot_ids()` waits until `m_rw_trx_hash_version` catches up
    to `m_max_trx_id`, then snapshots `rw_trx_hash` IDs and computes
    `min_trx_no`.
  - `trx_sys_t::get_new_trx_id_no_refresh()` increments `m_max_trx_id`.
- `mariadb/storage/innobase/include/read0types.h` and
  `mariadb/storage/innobase/read/read0read.cc`:
  - `ReadViewBase` uses `m_low_limit_id`, `m_up_limit_id`, active transaction
    IDs, and `m_low_limit_no` to decide visibility and purge safety.
  - `ReadView::open()` snapshots through `trx_sys`.

## Design

Add an internal `ownerless_trx_registry` primitive with:

- fixed header and fixed slot size suitable for a future `.shm` segment,
- latch-protected monotonic transaction ID allocation,
- active slot publication by owner process slot,
- atomic transaction serialisation-number allocation and publication for active
  transactions,
- slot generation checks on end,
- sorted active-ID snapshots for future read-view construction,
- read-view snapshots that return `next_trx_id` and `min_trx_no` using
  InnoDB's "unassigned serialisation number does not lower the purge limit"
  rule,
- active-count, next-ID, and oldest-active-ID helpers,
- dead-owner transaction cleanup by stable owner ID,
- cross-process tests proving ID allocation, active visibility, cleanup, and
  wait-free read helpers over a file-backed `MAP_SHARED` mapping.

The primitive records only transaction registry state. It does not attempt to
mirror InnoDB undo, read views, purge, redo, or record locks in this slice.

## Compatibility Impact

No public behavior changes. Ownerless read/write stays unavailable. The
compatibility matrix should mark this as internal primitive evidence only.

## Directory Impact

The implementation is a fixed segment in `concurrency/mylite-concurrency.shm`
after the process registry, wait channels, and MDL lock table. Dirty or invalid
`.shm` rebuilds reinitialize the transaction registry because the `.shm` file is
rebuildable coordination state, not durable truth.

Product opens validate the transaction-registry segment and release active
transaction entries for dead process-slot owners during process-slot cleanup.
The registry still has no durable commit, rollback, undo, purge, or read-view
authority, so product ownerless writers remain disabled.

## Tests

- Allocate transaction IDs across parent and child mappings.
- End transactions with generation checks and reject stale ends.
- Return a stable full result when all transaction slots are active.
- Allocate standalone IDs for non-active transaction-system paths.
- Atomically assign new transaction serialisation numbers to active
  transactions.
- Snapshot active transaction IDs, next transaction ID, oldest active ID, and
  read-view `min_trx_no`.
- Release all active transactions owned by a dead owner ID.
- Validate the fixed production `.shm` segment on open.
- Clean dead-owner transaction entries from the production `.shm` segment
  before process-slot reuse.
- Keep cleanup idempotent.

## Acceptance Criteria

- `libmylite.ownerless-primitives` covers the registry behavior.
- `libmylite.embedded-open-close` covers production `.shm` layout validation
  and dead-owner transaction cleanup on reopen.
- `ctest --preset embedded-dev --output-on-failure` passes.
- Docs clearly state that this does not enable product ownerless writes.
