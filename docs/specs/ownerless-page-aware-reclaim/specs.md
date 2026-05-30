# Ownerless Page-Aware Reclaim

## Problem

Ownerless page-version WAL reclamation previously had only two safe product
states: no live peers, or live peers with zero active page-version snapshot
pins. Any active repeatable-read or serializable snapshot pin blocked close-time
native checkpoint reclamation, even when part of the WAL prefix could be
proven unnecessary for that snapshot. That preserved correctness but left a
known WAL-growth and checkpoint-starvation performance gap.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- InnoDB page LSNs are written into page frames and dirty pages are published
  through the MyLite ownerless hooks after redo is assigned in
  `mariadb/storage/innobase/mtr/mtr0mtr.cc`.
- MyLite stores ownerless page versions in
  `packages/libmylite/src/ownerless_page_log.cc`; checkpoint currently rewrites
  retained records and publishes new page-index offsets through a callback.
- Product close-time reclamation is driven by
  `reclaim_ownerless_page_log_after_native_checkpoint()` in
  `packages/libmylite/src/database.cc`.
- Active snapshot LSNs are published in the page-pin registry implemented by
  `packages/libmylite/src/ownerless_page_pin_registry.cc`; the current product
  path can cheaply obtain the active count and oldest pinned read LSN.

## Design

Add a conservative page-log checkpoint primitive that can compact while an
oldest snapshot pin exists. The primitive keeps every record newer than the
oldest pinned LSN, and for any page whose checkpointed records would advance
past that oldest LSN, it also keeps the newest boundary record at or below the
oldest LSN. If that boundary is missing for any such page, the primitive returns
busy and leaves the WAL unchanged.

The product close path remains conservative for this slice: live peers with
active page-version pins still block native checkpoint reclamation. The new
primitive is deliberately lower-level proof machinery for the next product
slice, where close-time reclamation can call it only after the ownerless
lifecycle and dead-reader cleanup interaction is separately covered.

## Non-Goals

- This slice does not enable product active-pin reclamation yet. It defines and
  tests the safe page-log retention rule that product wiring must use later.
- This slice does not implement full per-pin pruning. Records newer than the
  oldest pin are retained so later active pins remain safe without enumerating
  every page-pin slot.
- This slice does not add durable DDL file-lifecycle metadata to page replay.
- This slice does not claim cross-process group commit optimization; safe
  serialization remains the current product behavior.

## Compatibility Impact

SQL visibility semantics are unchanged. Active repeatable-read and serializable
snapshot readers continue to observe their pinned read LSN. The change only
allows safe WAL compaction when the required page-version boundary evidence is
present.

## Directory And Lifecycle Impact

No new durable files or directory layout changes are introduced. Existing
product reclamation still operates only on `concurrency/mylite-concurrency.wal`,
the shared page-index segment in `mylite-concurrency.shm`, and the existing
checkpoint metadata when no active pins are present.

## Native Storage Impact

The existing product native InnoDB checkpoint still runs before WAL truncation
only when active pins are absent. The new primitive requires boundary proof
before invoking its prepare callback, which is the ordering product active-pin
reclamation must preserve before it can safely run a native checkpoint.

## Test Plan

- Add a page-log primitive test proving:
  - missing oldest-snapshot boundary returns busy and leaves the WAL readable;
  - present boundary compacts older records, retains the boundary, retains newer
    records, invokes the prepare callback exactly once, and drops independent
    old records made safe by the checkpoint.
- Keep normal ownerless SQL live snapshot-pin coverage to prove product
  behavior remains conservative while active-pin product wiring is not enabled.
- Run ownerless primitive, normal ownerless SQL, ownerless hook, formatting, and
  diff checks before committing.

## Acceptance Criteria

- The new primitive never invokes the native-prepare callback when boundary
  evidence is incomplete.
- Product active-pin reclamation remains disabled; zero-active-pin reclamation
  behavior is unchanged.
- Page-index replacement continues to receive retained record offsets after WAL
  rewrite.
- Existing ownerless crash, SQL, and stress coverage continues to pass.

## Risks

- Product active-pin reclamation still needs a separate lifecycle slice. An
  attempted product wiring during this slice exposed a live-peer dead-reader
  cleanup interaction and was intentionally not retained.
- Some SQL workloads will still be unable to reclaim while an active pin exists
  until product wiring can safely use the boundary-preserving primitive.
