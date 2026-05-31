# Ownerless Active-Reader Pressure

## Problem

Ownerless page-version reclamation now preserves active snapshot boundaries and
can use a single-active-pin primitive to compact checkpointed post-snapshot
records when boundary evidence is complete. A long repeatable-read ownerless
transaction can still be a pressure point: repeated writers may keep
`concurrency/mylite-concurrency.wal` non-empty until the reader exits, and some
workloads can dirty expanding undo/support or data-page sets.

Earlier active-reader pressure work added the bounded primitive needed for a
single active snapshot pin and product close-time use when exactly one
page-version pin exists. This slice adds bounded SQL pressure evidence for the
same area: a repeatable-read snapshot reader stays open while several
independent ownerless writer opens commit and close. The test proves the reader
keeps its original snapshot, the writer-visible state advances, WAL is retained
while the pin is live, and WAL is reclaimed after the reader releases the pin.
It does not claim a general WAL-size cap while a reader remains active.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` calls the MyLite page-version
  publish hook before the ownerless commit path flushes dirty pages, giving
  MyLite a chance to record the current native page as an older snapshot
  boundary.
- `packages/libmylite/src/database.cc` publishes shared page-version pins for
  ownerless repeatable-read and serializable snapshots, including
  `START TRANSACTION WITH CONSISTENT SNAPSHOT`.
- `reclaim_ownerless_page_log_after_native_checkpoint()` gates close-time
  reclamation through the shared pin registry. With live peers, it snapshots
  the page-version pin registry and calls a boundary-preserving page-log
  checkpoint path while pins are active.
- `publish_ownerless_snapshot_boundary_if_needed()` can append an older native
  page image as a boundary record when the native page LSN is at or below the
  oldest active pin.
- `packages/libmylite/src/ownerless_page_log.cc` has two active-pin retention
  modes: the general oldest-snapshot path preserves boundary records plus newer
  snapshot-page records, and the single-snapshot path can drop checkpointed
  post-snapshot records after native checkpoint proof because there is no later
  active reader that could need intermediate versions.
- Focused SQL pressure attempts that repeatedly update InnoDB rows under one
  reader still do not form a full bounded-size policy proof: each writer can
  dirty fresh undo/support records, and WAL may remain non-empty until the
  reader releases. That remains evidence for the broader pressure-policy gap.

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

Add a focused ownerless SQL selector, `active-reader-pressure`, and include the
same test in the normal ownerless SQL run. The test:

1. Creates the standard ownerless InnoDB fixture.
2. Forks a reader that starts a repeatable-read consistent snapshot and verifies
   the original aggregate.
3. Runs `MYLITE_OWNERLESS_ACTIVE_READER_PRESSURE_ROUNDS` ownerless writer
   opens, each committing one update and closing while the reader pin remains
   live.
4. Verifies every writer sees the advanced aggregate, the page-version WAL is
   not checkpointed while the pin is active, and at least one WAL record remains
   available.
5. Releases the reader, verifies the reader still sees the original aggregate,
   then verifies close-time reclamation checkpoints the WAL.
6. Reopens through ownerless and native exclusive modes, including forced
   `.shm` rebuild, to verify final data and checkpoint state.

The opt-in `ownerless-stress` preset runs the selector with a larger bounded
round count. This is still deterministic SQL coverage, not a replacement for
external MariaDB/RQG, unbounded long-reader pressure testing, or a user-visible
checkpoint pressure policy.

## Scope And Non-Goals

In scope:

- Single-snapshot page-log compaction semantics and product use for exactly one
  active page-version pin.
- Bounded same-row SQL pressure coverage while a repeatable-read snapshot pin is
  active.
- Verification that retained WAL is reclaimed after the reader releases and
  final data survives ownerless/native reopen plus forced `.shm` rebuild.

Out of scope:

- Background checkpointing, user-visible WAL size diagnostics, or a tunable WAL
  pressure limit.
- Handling missing boundary proof differently; that remains a deliberate safe
  no-reclaim result.
- Claiming bounded SQL-level WAL growth while a reader stays active.
- External MariaDB/RQG stress or long-running application-oracle pressure tests.

## Compatibility Impact

SQL isolation behavior is unchanged. The covered behavior is internal
storage-lifecycle behavior under existing repeatable-read snapshot semantics:
the reader keeps its pinned view, repeated peer commits advance writer-visible
state, and retained WAL is reclaimed after the pin is released.

## Directory And Lifecycle Impact

No new files or layout changes. The test exercises the existing
`concurrency/mylite-concurrency.wal`, `.ckpt`, and `.shm` lifecycle under
repeated live-reader pressure and verifies forced shared-memory rebuild.

## Native Storage Impact

No native storage format changes. The proof depends on existing page-version
WAL, native checkpoint evidence, native boundary synthesis, and page-index
rebuild behavior. If the native page image cannot prove a boundary at or below
the active pin, or if a workload keeps expanding the page set that needs
reconciliation, the conservative retained-WAL behavior remains separate work.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `active-reader-pressure` in `embedded-dev`.
- Build and run the same selector in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- A live repeatable-read snapshot reader continues to see its original
  aggregate after repeated peer writer commits.
- Each writer observes the advanced aggregate after its commit.
- Page-version WAL remains present while the reader pin is live and is
  checkpointed after the reader releases.
- Ownerless and native exclusive reopen, including forced `.shm` rebuild,
  preserve the final aggregate.
- Existing ownerless SQL, hook, and stress coverage remains green.

## Risks And Follow-Up

- This slice covers bounded repeated writes against one user row.
  Expanding page-set evidence is added by the
  `ownerless-expanding-page-pressure` slice; the first user-visible soft WAL
  limit is added by `ownerless-active-reader-pressure-limit`; checkpoint
  diagnostics, background reclamation, and external MariaDB/RQG long-running
  oracle stress remain separate work.
