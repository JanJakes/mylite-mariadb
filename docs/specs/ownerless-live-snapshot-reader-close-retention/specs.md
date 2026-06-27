# Ownerless Live Snapshot Reader-Close Reclaim

## Problem Statement

The live snapshot-pin reclaim SQL cases must prove two distinct boundaries:
while a repeatable-read reader owns a snapshot pin, peer page-version WAL must
remain retained; after the reader releases the pin, no-live reclaim may
checkpoint retained WAL if the native checkpoint proof can make the visible
boundary durable. Older reader-close coverage assumed the released reader must
always leave peer WAL for a later opener, but later native snapshot-boundary and
native checkpoint proof work made immediate no-live reader-close drain safe for
these focused cases.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `reclaim_ownerless_page_log_after_native_checkpoint()` still blocks live-peer
  reclaim while page-version pins are active and requires native checkpoint
  proof before truncating retained WAL.
- Native snapshot-boundary synthesis can leave compact boundary/native-support
  records that the final no-live reader close can prove and checkpoint after the
  snapshot releases.
- `docs/specs/ownerless-random-tx-rollback-handoff/specs.md` and
  `docs/specs/ownerless-cross-process-concurrency/specs.md` document the
  conservative rollback and handoff rules for cases that still cannot prove a
  safe native boundary.

## Design

Align `test_ownerless_live_snapshot_pin_blocks_page_log_reclaim()` and the
adjacent synthesized-boundary snapshot-pin case with the current reclaim
contract:

1. Keep the live reader pin while a peer writer appends page-version WAL.
2. Verify the writer close leaves WAL retained while the pin is active.
3. Release and reap the reader, then require retained WAL to checkpoint once no
   live pin remains and native proof succeeds.
4. Open a fresh ownerless read/write handle, verify the committed aggregate,
   and keep the final checkpoint assertion.
5. Force `.shm` rebuild and ordinary native reopen to prove the native boundary
   is durable without retained WAL.

No new file formats or SQL behavior are required for this adjustment.

## Scope

In scope:

- Focused production and hook SQL coverage for live snapshot reader-close
  retention, including the synthesized native-boundary case.
- Compatibility and cross-process concurrency documentation updates.

Out of scope:

- Page-aware active-pin pruning while a snapshot pin remains live.
- Broader DDL/file-lifecycle recovery.
- Native redo/checkpoint reconciliation beyond this reader-only handoff edge.
- External MariaDB/RQG stress.

## Compatibility Impact

SQL behavior and public API behavior are unchanged. Repeatable-read and
serializable readers still see the snapshot they pinned. The clarified contract
is lifecycle timing: a stale reader-only process that consumed peer WAL may
checkpoint it only after the pin releases and exact native page proof covers the
retained records; otherwise it leaves the WAL for the next safe recovery owner.

## Directory And Lifecycle Impact

No files or formats are added. Retained `concurrency/mylite-concurrency.wal`
records remain inside the MyLite database directory until a no-live ownerless
close or a later fresh ownerless opener can prove and checkpoint the visible
boundary.

## Native Storage Impact

Native InnoDB remains the durability authority after no-live checkpoint proof
has replayed/refreshed the retained page-version boundary and closed through the
existing native checkpoint path. The stale reader does not use newer native page
LSNs as successor proof for peer-written WAL; it checkpoints only when retained
payloads exactly match native pages or otherwise remain proven by the native
checkpoint proof scan.

## Test Plan

- Reproduce the old failures with
  `mylite_ownerless_cross_process_sql_test sql-case test_ownerless_live_snapshot_pin_blocks_page_log_reclaim`
  and
  `mylite_ownerless_cross_process_sql_test sql-case test_ownerless_live_snapshot_pin_synthesizes_page_boundary`.
- Build `mylite_ownerless_cross_process_sql_test` with `php-embedded-prod`.
- Run the focused production SQL case and the `live-reclaim` selector.
- Build and run the focused hook-build SQL case and adjacent hook selectors
  that cover active-pin reclaim boundaries.
- Run relevant ownerless stress or shard checks if the focused selector exposes
  broader instability.
- Run `format-check-prod`, `git diff --check`, and staged diff checks before
  commit.

## Acceptance Criteria

- The live snapshot pin retains peer WAL until the pin releases.
- After reader release, no-live reclaim checkpoints retained WAL before or by
  the next ownerless close.
- A fresh ownerless opener reads the committed data with the WAL already
  checkpointed or checkpoints it again as a no-op.
- Forced `.shm` rebuild plus ordinary native reopen still preserve the final
  committed rows without retained WAL.
- Existing live idle-peer, live writer, synthesized-boundary, active-pin
  boundary, and killed-pin reclaim cases continue to pass.

## Risks And Open Questions

- This does not reduce WAL growth for long-running active pins; it only makes
  the post-release reader-only handoff contract explicit.
- Broader redo/checkpoint, DDL/file-lifecycle, transaction crash, pressure
  crash/oracle, and external randomized stress gates remain open completion
  items.
