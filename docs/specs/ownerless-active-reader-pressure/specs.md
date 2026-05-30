# Ownerless Active Reader Pressure

## Problem

Ownerless page-version reclamation now preserves active snapshot boundaries, but
a long repeatable-read ownerless transaction can still be a pressure point. If
every peer writer close retained each post-snapshot page image while that reader
stays open, `concurrency/mylite-concurrency.wal` could grow until the reader
exits even when native boundary proof is available.

This slice added the bounded primitive needed for a single active snapshot pin
to compact checkpointed post-snapshot page records. The
`ownerless-single-active-pin-reclaim` follow-on now enables that mode in product
close-time reclaim when exactly one active page-version pin exists. It still
does not claim SQL-level repeated-writer pressure coverage; explored SQL
workloads can dirty an expanding undo/support-page set and keep growing the WAL
while the reader stays open.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` calls the MyLite page-version
  publish hook before the ownerless commit path flushes dirty pages, giving
  MyLite a chance to record the current native page as an older snapshot
  boundary.
- `packages/libmylite/src/database.cc` runs
  `reclaim_ownerless_page_log_after_native_checkpoint()` on final runtime
  close. With live peers, that path snapshots the page-version pin registry and
  calls the boundary-preserving page-log checkpoint path when active pins
  exist.
- `packages/libmylite/src/ownerless_page_log.cc` preserves newer snapshot-page
  records for the general multi-pin path, and has a single-snapshot checkpoint
  mode that can drop checkpointed post-snapshot records once native checkpoint
  proof has been prepared.
- `packages/libmylite/src/database.cc` can synthesize a boundary record from a
  native tablespace page during page-version publish when the page LSN is at or
  below the oldest active pin.
- Focused SQL pressure attempts that repeatedly updated InnoDB rows under one
  reader did not form a bounded product proof: each writer dirtied fresh
  undo/support records, and retained WAL size continued to grow. That remains
  evidence for the broader active-reader pressure gap, not a passing
  compatibility claim.

## Design

The active-reader pressure policy remains conservative:

- no background checkpoint worker is introduced,
- no hard size cap aborts user SQL,
- close-time reclaim runs when an ownerless non-read-only runtime closes,
- active pins allow reclaim only when every snapshot-sensitive page advanced
  after the relevant pin has a retained boundary image,
- multi-pin reclaim preserves newer checkpointed snapshot-page records because
  later active readers can still need intermediate versions,
- single-pin reclaim can drop checkpointed post-snapshot records because no
  later active reader exists, and
- if boundary proof is missing, the WAL stays unchanged.

The primitive now has two active-pin retention modes:

- the existing oldest-snapshot mode preserves boundary records plus newer
  snapshot-page records, which remains necessary when multiple active pins may
  need intermediate versions;
- the new single-snapshot mode preserves required boundary records but can drop
  checkpointed post-snapshot records because the caller has proved there is no
  later active pin.

Product close-time reclaim now uses the single-snapshot primitive only when the
pin registry snapshot reports exactly one active page-version pin. It keeps the
general oldest-snapshot mode for two or more active pins, where later active
readers may still need intermediate versions.

## Scope And Non-Goals

In scope:

- Single-snapshot page-log compaction semantics and product use for exactly one
  active page-version pin.
- Documentation that SQL-level repeated-writer pressure remains broader work.

Out of scope:

- Background checkpointing, user-visible WAL size diagnostics, or a tunable WAL
  pressure limit.
- Handling missing boundary proof differently; that remains a deliberate safe
  no-reclaim result.
- Claiming bounded SQL-level repeated-writer pressure under a live reader.
- External MariaDB/RQG stress or long-running application-oracle pressure tests.

## Compatibility Impact

SQL isolation behavior is unchanged. The covered behavior is an internal
storage-lifecycle primitive: when exactly one active page-version pin exists
and boundary proof is complete, a caller can drop checkpointed post-snapshot
records instead of retaining them for hypothetical later pins.
Repeated SQL writer pressure remains a documented partial-compatibility gap.

## Directory And Lifecycle Impact

No new files or layout changes are introduced. The slice only exercises
existing `concurrency/mylite-concurrency.wal`,
`concurrency/mylite-concurrency.ckpt`, and
`concurrency/mylite-concurrency.shm` state.

## Native Storage Impact

The proof depends on native checkpoint evidence and native boundary synthesis.
If the native page image cannot prove a boundary at or below the active pin, or
if the SQL workload keeps expanding the page set that needs reconciliation, the
existing conservative behavior can leave the WAL retained and remains a
separate pressure gap.

## Test Plan

- Add focused primitive coverage for the new single-snapshot checkpoint mode
  while keeping the existing multi-pin-preserving behavior intact.
- Run focused ownerless primitive coverage.
- Run the embedded `live-reclaim` selector and hook active-pin subset to prove
  the existing product close-time page-log paths are not regressed.
- Run the opt-in ownerless stress preset, `format-check`, and
  `git diff --check`.

## Acceptance Criteria

- The existing oldest-snapshot primitive still retains newer snapshot-page
  records.
- The single-snapshot primitive retains the required boundary record and drops
  checkpointed post-snapshot records.
- Product close-time reclaim uses that single-snapshot primitive when exactly
  one active pin exists.
- Existing live-reclaim and active-pin hook coverage continue to pass.

## Risks

- The slice deliberately does not solve SQL-level pressure for workloads that
  dirty fresh undo/support or snapshot-sensitive page classes on every writer
  close.
- This is not a replacement for external long-running stress with a MariaDB or
  RQG oracle.
