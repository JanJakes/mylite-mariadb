# Ownerless Product Transaction Binding

## Problem

The ownerless transaction registry and InnoDB transaction hooks were proven
separately. Product `mylite_open()` still needed to bind normal persistent
database opens to the directory-owned transaction registry so ordinary InnoDB
SQL uses shared transaction IDs, active transaction publication, transaction
serialisation numbers, and read-view snapshots.

This slice binds the existing hook surface to
`concurrency/mylite-concurrency.shm`. It does not enable
`MYLITE_CAP_OWNERLESS_RW`, remove the exclusive directory lock, or claim full
cross-process write safety.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/trx0sys.h` keeps the recovered next
  transaction ID in process-local `trx_sys_t::m_max_trx_id`.
- Product binding must not reset the shared registry to `1` after InnoDB
  recovery. The shared registry next-ID floor is raised from the recovered
  local `trx_sys` value before hooks are installed.
- `packages/libmylite/src/database.cc` owns the runtime mapping of
  `concurrency/mylite-concurrency.shm`, process-slot allocation, hook install,
  and hook reset on final close.

## Design

- Map the production `.shm` file as before, but require the mapping to include
  the transaction-registry segment.
- Allocate the owner process slot before installing hooks.
- Seed the transaction registry with at least InnoDB's recovered local
  `m_max_trx_id`.
- Install MDL and transaction hooks together once the process slot and shared
  transaction registry are ready.
- Implement transaction hooks as thin registry adapters:
  - standalone ID allocation calls `mylite_ownerless_trx_registry_allocate_id()`,
  - read-write registration calls `mylite_ownerless_trx_registry_begin()`,
  - serialisation-number assignment calls
    `mylite_ownerless_trx_registry_assign_new_no()`,
  - deregistration calls `mylite_ownerless_trx_registry_end_by_id()` using the
    process owner ID,
  - read-view snapshots call
    `mylite_ownerless_trx_registry_snapshot_read_view()`.
- Reset hooks before unmapping shared memory.
- Release any remaining MDL or transaction entries for the process owner before
  releasing the process slot.

## Compatibility Impact

No public capability changes. Persistent MyLite opens still use the exclusive
directory lock, so cross-process write mode remains unavailable. The change is
internal evidence that the production runtime can use directory-owned
transaction identity for normal InnoDB SQL.

## Directory And Lifecycle Impact

No durable layout change. The existing `.shm` transaction-registry segment now
becomes active during persistent runtime opens. `:memory:` databases do not use
the persistent concurrency directory and keep the previous process-local path.

## Test Plan

- Extend `libmylite.embedded-open-close` to create an InnoDB table through
  normal `mylite_open()`, run commit and rollback transactions, and assert that
  the production shared transaction registry:
  - publishes an active transaction while DML is open,
  - returns to zero active transactions after commit and rollback,
  - advances transaction IDs through the shared registry,
  - advances registry generation through register/deregister,
  - remains clean after close.
- Keep the existing dead-owner cleanup test compatible with a seeded next-ID
  floor instead of assuming the registry starts at transaction ID `1`.
- Run focused open/close and ownerless transaction tests.
- Run the full `embedded-dev` test suite.

## Acceptance Criteria

- Normal persistent InnoDB SQL reaches the production transaction registry.
- No hook pointer remains installed after final close.
- Existing directory layout and `:memory:` behavior remain unchanged.
- Ownerless read/write capability remains disabled until record locks, page
  visibility, redo/checkpoint coordination, purge, and recovery are complete.
