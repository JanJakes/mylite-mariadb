# Ownerless InnoDB Lock Hooks

## Problem

The ownerless InnoDB lock registry now models MariaDB-compatible table and
record-lock conflicts in mapped shared memory, but InnoDB still creates,
grants, and removes its native `lock_t` objects in process-local memory. The
registry is not useful for SQL execution until real InnoDB lock lifecycle
events publish and release directory-owned entries.

This slice wires a guarded hook bridge from InnoDB table and record locks into
the MyLite ownerless lock registry. It is still not a product ownerless
read/write enablement slice: MyLite keeps the exclusive directory lock and does
not advertise `MYLITE_CAP_OWNERLESS_RW`.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/lock0types.h` defines `lock_t`,
  `LOCK_TABLE`, `LOCK_WAIT`, table modes, record modes, record flags, and the
  record page key stored in `lock_t::un_member.rec_lock.page_id`.
- `mariadb/storage/innobase/include/lock0priv.inl` defines
  `lock_rec_set_nth_bit()`, which is the common point where explicit record
  lock bitmap bits become active.
- `mariadb/storage/innobase/include/lock0priv.h` defines
  `lock_rec_reset_nth_bit()`, which clears explicit record lock bitmap bits.
- `mariadb/storage/innobase/lock/lock0lock.cc` creates record lock objects in
  `lock_rec_create()`, grants waiting lock requests in `lock_grant()`, removes
  full record lock objects in `lock_rec_dequeue_from_page()`, discards record
  lock objects in `lock_rec_discard()`, creates table locks in
  `lock_table_create()`, and removes table locks in `lock_table_remove_low()`.
- `mariadb/storage/innobase/include/dict0mem.h` stores stable InnoDB table and
  index identifiers in `dict_table_t::id` and `dict_index_t::id`.
- `mariadb/storage/innobase/include/trx0trx.h` stores the InnoDB transaction ID
  in `trx_t::id`, but MariaDB can hold locking-read record locks before that
  ID is assigned.
- `mariadb/storage/innobase/trx/trx0trx.cc` resets reusable `trx_t` objects in
  `trx_init()`, which is the lifecycle boundary used to clear MyLite's
  transient lock-registry transaction identity.

## Design

- Add an InnoDB-side MyLite hook bridge with a small C-callable surface:
  - install/reset/has hook lifecycle,
  - table-lock publish/release from `lock_t`,
  - record-lock bitmap-bit publish/release from `lock_t`,
  - whole-record-lock release for object-removal paths.
- Keep MariaDB lock compatibility rules in the first-party
  `ownerless_innodb_lock_registry` primitive. The InnoDB hook bridge only
  normalizes native `lock_t` identifiers, modes, and flags.
- Use a stable lock-registry transaction identity for every published lock. If
  InnoDB has already assigned `trx_t::id`, use it. If a locking read holds
  locks before `trx_t::id` exists, assign a MyLite transient lock identity on
  `trx_t` and keep using it until `lock_release()` or `lock_release_on_drop()`
  releases the transaction's locks.
- Publish only granted locks. Waiting locks stay unpublished until
  `lock_grant()` clears `LOCK_WAIT`, then the bridge publishes all lock bits or
  the table lock.
- Release only granted locks. Waiting cancellation removes ungranted local
  requests and must not remove directory-owned entries.
- Ignore spatial predicate/page predicate locks for now, matching the primitive
  non-goal.
- Treat hook failures as InnoDB fatal assertions. While the product exclusive
  lock remains active, any registry conflict or unbalanced release is evidence
  of hook or lifecycle corruption, not a recoverable application lock wait.

## Scope

- Add the hook bridge source and build wiring.
- Expand `concurrency/mylite-concurrency.shm` with a fixed InnoDB lock-registry
  segment and install product hooks against it while the directory lock is
  still exclusive.
- Add SQL coverage proving real InnoDB transactions publish lock entries during
  a held write transaction and release them on commit, rollback, close, and
  dead-owner cleanup.
- Update compatibility documentation to distinguish primitive coverage from
  product ownerless read/write support.

## Non-Goals

- Do not expose `MYLITE_CAP_OWNERLESS_RW`.
- Do not remove the exclusive product directory lock.
- Do not implement cross-process InnoDB wait/deadlock handling in this slice.
- Do not support spatial predicate locks in the ownerless lock registry.
- Do not solve page visibility, buffer-pool coordination, redo/checkpoint
  ownership, or crash recovery in this slice.

## Compatibility Impact

No public capability changes. The hook bridge makes InnoDB native lock state
visible to MyLite directory-owned coordination state while preserving MariaDB's
native in-process lock behavior.

## Directory And Lifecycle Impact

The `.shm` format gains an InnoDB lock-registry segment. It is volatile
coordination state bound to the database UUID and rebuilt with the rest of the
shared-memory layout when the format changes or stale dirty state is detected.
Dead process-slot cleanup releases all lock entries owned by that process slot.

## Native Storage Impact

No native engine files or durable formats change. The bridge observes InnoDB
table and record locks and mirrors them into MyLite volatile shared memory.

## Test Plan

- Extend the shared-memory layout test to validate the new lock-registry
  segment descriptor and header.
- Add embedded SQL coverage that:
  - opens a real MyLite database,
  - creates and updates an InnoDB table inside a transaction,
  - exercises DDL that takes locking-read record locks before a durable InnoDB
    transaction ID exists,
  - observes non-zero ownerless lock-registry entries while the transaction is
    open,
  - verifies those entries are gone after commit and rollback,
  - verifies hook reset on close.
- Extend dead-owner cleanup coverage to seed a stale InnoDB lock entry and
  prove the next open removes it.
- Keep primitive compatibility tests as the detailed table/record conflict
  source.

## Acceptance Criteria

- Real InnoDB table/record lock lifecycle events publish and release registry
  entries through guarded hooks.
- Locks acquired before `trx_t::id` exists keep a stable MyLite lock identity
  through release.
- Waiting locks are not published until granted.
- Commit, rollback, close, and dead-owner cleanup leave no lock-registry
  entries for the current process slot.
- The `.shm` layout version, segment table, and validation logic match the new
  segment.
- Full embedded-dev tests pass.

## Risks And Follow-Up

- The fixed-size lock registry is not sufficient for unlimited product
  ownerless write mode. Product enablement needs growable or partitioned
  directory-owned lock storage.
- This slice mirrors granted local locks. Product ownerless writers still need
  an external-conflict wait path before local grant, a directory-owned
  wait-for/deadlock protocol, page visibility, and redo/checkpoint ownership.
- Transient MyLite lock identities are enough to balance and conflict-check
  locks that precede `trx_t::id`; full ownerless writer support still needs the
  wait-for/deadlock protocol to map every waiting edge to a stable directory
  transaction owner.
