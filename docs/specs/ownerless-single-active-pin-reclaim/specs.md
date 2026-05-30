# Ownerless Single Active Pin Reclaim

## Problem

Ownerless close-time page-log reclamation can reclaim with active snapshot pins
only when every snapshot-sensitive page has a boundary image at or before the
oldest pin. The page-log primitive already distinguishes multi-pin retention
from single-snapshot retention, but before this slice product close-time reclaim
used the multi-pin path for every active-pin case.

That is conservative but leaves avoidable WAL pressure when exactly one active
repeatable-read snapshot is open: post-snapshot page records that are covered by
a native checkpoint are retained even though no later active pin can need those
intermediate versions.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:reclaim_ownerless_page_log_after_native_checkpoint()`
  snapshots the ownerless page-version pin registry on non-read-only close.
  Before this slice, if live peers had active pins, it called
  `mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at()` for all
  active-pin counts.
- `packages/libmylite/src/ownerless_page_log.cc` implements both
  `mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at()` and
  `mylite_ownerless_page_log_checkpoint_preserving_single_snapshot_at()` through
  one shared checkpoint implementation. The oldest-snapshot mode keeps
  checkpointed post-snapshot records for later pins; the single-snapshot mode
  keeps required boundary records and drops checkpointed post-snapshot records.
- `packages/libmylite/src/database.cc:publish_ownerless_snapshot_boundary_if_needed()`
  can synthesize a missing boundary record during page-version publish when an
  older native page image can prove `page_lsn <= oldest_pin_lsn`.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` already has
  SQL coverage for live active-pin blocking, native boundary synthesis, killed
  pin cleanup, and hook-only multi-pin active boundary reclamation.
- A reader that creates a new snapshot after a writer has published newer
  ownerless redo state can pin a live latest/visible LSN that is ahead of the
  durable `.ckpt` visible LSN while older active pins remain. The single-pin
  reclaim path must therefore drop only post-snapshot records that are covered
  by the native checkpoint visible LSN and retain newer complete records.
  Existing pins that need an intermediate read LSN are counted in the registry
  snapshot taken before reclaim.

## Scope And Non-Goals

- Use the single-snapshot page-log checkpoint mode when close-time reclaim sees
  exactly one active page-version pin.
- Keep the existing oldest-snapshot mode when two or more pins are active.
- Keep live-boundary-synthesis SQL coverage for partial native boundary proof
  and add hook-backed product coverage for the one-pin path that drops
  checkpointed post-snapshot records when proof is complete.
- Keep existing multi-pin coverage to prove later-pin retention is not
  regressed.
- Do not add background checkpointing, hard WAL size limits, user-visible WAL
  diagnostics, or external MariaDB/RQG pressure stress.
- Do not claim the broader repeated-SQL-writer pressure gap is fully solved;
  workloads that keep dirtying fresh undo/support or snapshot-sensitive page
  sets can still retain WAL until boundary proof and checkpoint progress catch
  up.

## Design

- In `reclaim_ownerless_page_log_after_native_checkpoint()`, branch on
  `active_pin_count == 1`.
- For one active pin, call
  `mylite_ownerless_page_log_checkpoint_preserving_single_snapshot_at()` with
  the current visible LSN and oldest pin LSN.
- For two or more active pins, keep the existing
  `mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at()` call.
- Reuse the existing native-checkpoint prepare callback and page-index
  replacement callback for both paths.
- Reuse the ownerless SQL test harness WAL record counter to prove, in the
  hook-backed active-pin boundary selector, that once only one active pin
  remains, every retained WAL record covered by the native checkpoint is at or
  before the pinned snapshot LSN.
- Keep the live boundary-synthesis SQL selector as coverage that product page
  publishing can synthesize native page boundaries while still preserving the
  safe no-reclaim result when other snapshot-sensitive pages lack proof.

## Compatibility Impact

SQL isolation behavior is unchanged. The slice improves close-time storage
pressure for the covered one-active-pin case while preserving the existing
multi-pin retention rule.

## Directory And Lifecycle Impact

No new files or layout changes. The slice changes how existing
`concurrency/mylite-concurrency.wal` records are compacted during ownerless
runtime close.

## Native Storage Impact

The reclaim still runs only after native checkpoint preparation proves the
visible LSN is covered. Missing boundary proof remains a no-reclaim result.

## Binary Size And Dependencies

No dependency or license changes. Binary-size impact is limited to one branch
and test code.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `live-reclaim` in `embedded-dev`.
- Build and run focused `live-reclaim` plus `active-pin-reclaim-boundary` in
  `ownerless-test-hooks`.
- Run focused ownerless primitive tests for page-log single/multi snapshot
  retention.
- Run embedded ownerless cross-process SQL, hook ownerless SQL label, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- Single active snapshot-pin close-time reclaim retains no checkpointed
  post-snapshot page-version records when boundary proof is complete; required
  boundary retention remains covered by primitive and native boundary-synthesis
  tests.
- Multi-pin active reclaim continues to retain the records needed by later
  active pins.
- Existing live-reclaim, active-pin hook, primitive, full ownerless SQL, hook,
  and stress verification remains green.

## Risks And Follow-Up

- The registry snapshot does not freeze future pin creation. This is acceptable
  for the one-pin optimization because a later newly created pin observes live
  latest/visible ownerless redo state while close-time reclaim only drops page
  records covered by the durable native checkpoint visible LSN; already-existing
  intermediate pins are counted before reclaim.
- Repeated writer pressure with expanding page sets remains broader work, as do
  external MariaDB/RQG or long-running application-oracle stress runs.
