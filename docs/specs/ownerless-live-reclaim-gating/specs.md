# Ownerless Live Reclaim Gating

## Problem Statement

MyLite now reclaims ownerless page-version WAL records on close after native
checkpoint evidence, including partial compaction when newer records remain. The
path is still limited to the no-peer proof. That leaves page-version WAL growth
when live ownerless peers remain open even if those peers are idle or only using
statement-scoped reads.

This slice defines and implements the first safe live-peer reclamation gate:
allow close-time reclamation with live peers only when no peer has an active
MyLite page-version snapshot pin. It deliberately does not prune WAL records
needed by an active repeatable-read/serializable snapshot.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `packages/libmylite/src/database.cc`
  - `refresh_ownerless_external_pages_before_statement()` computes
    `page_version_read_lsn` from the shared ownerless latest/visible LSNs. For
    repeatable-read and serializable explicit transactions, it stores the first
    consistent-read LSN in `mylite_db::ownerless_transaction_snapshot_visible_lsn`
    and reuses that value for later statements.
  - `update_ownerless_transaction_state_after_successful_sql()` pins
    `ownerless_transaction_snapshot_visible_lsn` for `START TRANSACTION WITH
    CONSISTENT SNAPSHOT` using `ownerless_observed_visible_lsn`, and clears that
    process-local pin on transaction end.
  - `rollback_active_transaction()` clears the process-local snapshot state when
    close or deadlock handling rolls back an open transaction.
  - `reclaim_ownerless_page_log_after_native_checkpoint()` still requires
    `ownerless_runtime_has_no_live_peers()` before native checkpoint proof and
    page-log compaction.
- `mariadb/storage/innobase/read/read0read.cc`
  - `ReadView::publish_ownerless()` calls
    `mylite_ownerless_read_view_register()` with InnoDB transaction visibility
    fields.
  - `ReadView::unpublish_ownerless()` calls
    `mylite_ownerless_read_view_deregister()`.
  - These hooks cover InnoDB purge/read-view visibility, but they do not expose
    MyLite page-version read LSNs.
- `packages/libmylite/src/ownerless_read_view_registry.cc`
  - The shared read-view registry stores `low_limit_id`, `low_limit_no`, and
    active transaction IDs. It has no field for page-version read LSN.
- `packages/libmylite/src/ownerless_page_log.cc`
  - Prefix checkpointing drops records at or below `safe_commit_lsn`. A live
    snapshot pinned at LSN `N` can still need old page images at or below `N`
    when native disk pages have advanced beyond `N`; therefore the current
    prefix checkpoint primitive is unsafe while any page-version snapshot pin is
    active.

## Design

Add a first-party shared page-version pin registry in
`concurrency/mylite-concurrency.shm`.

The registry records:

- fixed slot count and slot size,
- generation and active count,
- per-slot owner id and owner generation,
- per-slot page-version read LSN.

The first implementation uses the registry as a safe gate:

1. Register a shared page-version pin when a MyLite ownerless connection pins a
   repeatable-read or serializable page-version snapshot.
2. Release the pin when the explicit transaction ends, rolls back during close,
   or the process owner is cleaned up after death.
3. Allow close-time page-log reclamation with live peers only when the page-pin
   registry has zero active pins.
4. Keep the existing native checkpoint proof and partial compaction logic
   unchanged after the gate passes.

The registry stores the pinned LSN even though this slice only checks active
count. The value is reserved for a later page-aware pruning slice that can retain
the latest needed page version for each active snapshot instead of blocking all
live reclamation.

## Implementation Status

Implemented in `packages/libmylite/src/ownerless_page_pin_registry.*` and
`packages/libmylite/src/database.cc`.

The product path now:

- adds a page-pin registry segment to `mylite-concurrency.shm`,
- registers shared pins for repeatable-read/serializable page-version snapshot
  LSNs, including a pre-execution pin for `START TRANSACTION WITH CONSISTENT
  SNAPSHOT` so close-time reclamation cannot race between native snapshot
  creation and shared pin publication,
- releases pins on transaction end, rollback/close, and dead-owner cleanup,
- permits close-time reclamation with live peers only when the registry reports
  zero active pins,
- refreshes the closing runtime's external clean page state before native
  checkpoint proof so a former snapshot reader cannot reclaim from a stale
  buffer-pool view after it becomes the last closer.

Primitive tests cover same-process, cross-process, owner-cleanup, and
slot-exhaustion behavior. Ownerless SQL tests cover live idle-peer reclamation,
live snapshot-pin blocking, and an unsafe-hook pause after consistent-snapshot
pre-pinning but before SQL execution.

## Why Prefix Checkpointing Cannot Use Oldest Pinned LSN

If a transaction pins page-version visibility at LSN `N`, it may need the latest
WAL image at or below `N` for any page whose native disk frame has advanced past
`N`. Calling `mylite_ownerless_page_log_checkpoint_at(..., N, ...)` would remove
exactly those records. A safe live-reader pruning algorithm must be page-aware,
not a simple commit-LSN prefix truncation.

This slice therefore treats any active page-version snapshot pin as a hard block
for prefix checkpointing.

## Scope

In scope:

- A shared MyLite page-version pin registry primitive.
- Shared-memory segment layout for that registry.
- Process-owner cleanup for dead owners' pins.
- Product integration for explicit transaction snapshot pins.
- Close-time reclamation with live peers when the registry has no active pins.
- Tests proving active pins block live-peer reclamation and idle live peers do
  not.

Out of scope:

- Page-aware pruning while active snapshots exist.
- Background reclamation independent of close.
- Changing SQL isolation semantics.
- Replacing the InnoDB read-view registry.

## Compatibility Impact

SQL behavior does not change. Repeatable-read and serializable transactions keep
their current page-version snapshot behavior. The change only enables reclaiming
page-version WAL bytes in cases where live peers have no durable page-version
snapshot dependency.

Compatibility remains partial until page-aware active-reader pruning or an
explicit policy for long-running snapshot pressure is implemented.

## Directory And Lifecycle Impact

The new registry is volatile shared-memory state under
`concurrency/mylite-concurrency.shm`. It does not add durable files. Dirty,
invalid, incompatible, or no-live-process stale `.shm` rebuilds clear the
registry, consistent with other ownerless volatile coordination segments.

Dead-owner cleanup releases pins for owners whose process slot can be cleaned.
If cleanup cannot prove owner death, pins remain and reclamation stays blocked.

## Native Storage Impact

The native checkpoint proof remains unchanged. The new registry only decides
whether live peers can safely use that proof. It does not change native InnoDB
page, redo, or tablespace formats.

## Public API And Layout Impact

No public `libmylite` API changes. The ownerless `.shm` layout version and
segment table change because a new internal segment is added.

## Binary Size Impact

The implementation adds one small first-party registry primitive and tests. It
does not add dependencies or new MariaDB source hooks.

## Test Plan

- Primitive tests for open/close, oldest-LSN snapshot, owner cleanup, slot
  exhaustion, and cross-process visibility.
- SQL hook test with a live idle ownerless peer proving close-time reclamation
  can shrink/checkpoint the WAL while the peer remains open.
- SQL hook test with a live repeatable-read or consistent-snapshot transaction
  proving reclamation stays blocked until that transaction releases its pin, and
  then succeeds.
- Unsafe SQL hook test pausing after a `START TRANSACTION WITH CONSISTENT
  SNAPSHOT` page-version pin is published but before the SQL executes, proving a
  concurrent closer cannot reclaim the WAL during that publication window.
- Embedded and hook ownerless SQL CTest, ownerless stress, `format-check`, and
  `git diff --check`.

## Acceptance Criteria

- Active page-version snapshot pins are visible across ownerless processes.
- Dead-owner cleanup releases pins for dead owners.
- Close-time reclamation no longer requires no-live-peers when the pin registry
  is empty.
- Close-time reclamation does not compact WAL while an active page-version
  snapshot pin exists.
- Existing ownerless concurrency, crash, and stress tests remain green.

## Risks And Open Questions

- Long-running repeatable-read snapshots can still block reclamation. That is
  correct for this slice but needs an explicit pressure policy later.
- A later page-aware pruning slice must use the stored LSN values carefully; a
  commit-LSN prefix checkpoint at the oldest pinned LSN is unsafe.
