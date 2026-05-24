# Ownerless Transaction Registry

## Problem

Ownerless write concurrency cannot rely on each embedded MariaDB process having
its own private InnoDB transaction system. Cross-process MVCC needs a
directory-owned source of transaction IDs and a visible set of active
read-write transactions before read views, purge limits, lock ownership, redo,
and commit publication can be made correct.

This slice adds the first MyLite-owned transaction-registry primitive. It does
not hook MariaDB yet and does not enable `MYLITE_CAP_OWNERLESS_RW`.

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
- slot generation checks on end,
- sorted active-ID snapshots for future read-view construction,
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

The first implementation is a standalone internal primitive covered by tests.
The follow-up slice will add the registry as a fixed segment in
`concurrency/mylite-concurrency.shm` after the primitive is proven.

## Tests

- Allocate transaction IDs across parent and child mappings.
- End transactions with generation checks and reject stale ends.
- Return a stable full result when all transaction slots are active.
- Snapshot active transaction IDs, next transaction ID, and oldest active ID.
- Release all active transactions owned by a dead owner ID.
- Keep cleanup idempotent.

## Acceptance Criteria

- `libmylite.ownerless-primitives` covers the registry behavior.
- `ctest --preset embedded-dev --output-on-failure` passes.
- Docs clearly state that this does not enable product ownerless writes.
