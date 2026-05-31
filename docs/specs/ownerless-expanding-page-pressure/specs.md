# Ownerless Expanding Page Pressure

## Problem

Ownerless active-reader pressure coverage proves repeated same-row commits while
a repeatable-read snapshot pin is live. That is useful, but it does not exercise
the broader pressure shape where each writer dirties a different large row and
therefore expands the data-page set that active-pin reclamation must preserve or
prove with native boundary images.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/include/trx0trx.h` defines repeatable-read and
  serializable transactions as consistent-read snapshot users.
- `packages/libmylite/src/database.cc` publishes ownerless page-version pins
  for repeatable-read and serializable ownerless transactions on first
  consistent read, including `START TRANSACTION WITH CONSISTENT SNAPSHOT`.
- `reclaim_ownerless_page_log_after_native_checkpoint()` snapshots the
  page-version pin registry before live-peer close-time reclamation and uses the
  boundary-preserving page-log path when pins are active.
- `publish_ownerless_snapshot_boundary_if_needed()` can synthesize an older
  page boundary from the native tablespace page when the native page LSN proves
  it is at or below the oldest active pin.
- The existing `active-reader-pressure` SQL selector repeatedly updates one row
  under a live reader and records that broader expanding page-set pressure still
  needs evidence.

## Design

Add a bounded SQL selector, `expanding-page-pressure`, that:

1. Creates an InnoDB table with large `VARBINARY(4000)` rows.
2. Starts a child ownerless connection with a repeatable-read consistent
   snapshot over the table.
3. Updates distinct large rows through separate ownerless writer opens while
   the reader pin stays live.
4. Verifies every writer sees the advanced aggregate and the page-version WAL
   remains retained while the reader is active.
5. Releases the reader, verifies its original snapshot was stable, and checks
   that close-time reclamation checkpoints the WAL.
6. Reopens through ownerless and native exclusive modes, including forced
   `.shm` rebuild, to verify final data and checkpoint state.

The normal selector uses a small row count. The opt-in `ownerless-stress`
preset runs the same selector with a larger bounded row count. This is evidence
for expanding page-set pressure, not a user-visible WAL size cap.

## Scope And Non-Goals

In scope:

- SQL-level expanding data-page pressure while one repeatable-read snapshot pin
  remains active.
- Verification that page-version WAL is retained during the pin and reclaimed
  after release.
- Ownerless and native exclusive reopen checks after forced `.shm` rebuild.

Out of scope:

- Background checkpoint workers, WAL-size diagnostics, or hard pressure limits.
- External MariaDB/RQG oracle stress.
- Durable DDL/file-lifecycle replay beyond existing table files.
- Changing native storage formats or the page-version WAL format.

## Compatibility Impact

SQL behavior is unchanged. This slice strengthens evidence for existing
repeatable-read snapshot semantics under ownerless peer writes. The reader keeps
its original snapshot, writers observe committed progress, and the final state
is durable through the documented MyLite directory lifecycle.

## Directory And Lifecycle Impact

No layout changes. The test exercises existing
`concurrency/mylite-concurrency.wal`, `.ckpt`, and `.shm` state while a live
snapshot pin forces page-version retention across a growing set of data pages.

## Native Storage Impact

No native format changes. The proof depends on existing native InnoDB pages,
page-version WAL records, native checkpoint evidence, and boundary synthesis.
If a later workload cannot prove all required boundaries, the conservative
no-reclaim behavior remains correct.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `expanding-page-pressure` in `embedded-dev`.
- Build and run the selector in `ownerless-test-hooks`.
- Run embedded ownerless SQL, hook ownerless SQL, ownerless stress,
  `format-check`, and diff checks.

## Acceptance Criteria

- The snapshot reader sees the original aggregate before and after peer writes.
- Each distinct-row writer sees the advanced aggregate after commit.
- Page-version WAL remains present while the snapshot pin is live.
- Page-version WAL checkpoints after the reader releases.
- Ownerless and native exclusive reopen preserve the final aggregate before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- The stress row count is bounded and deterministic. External long-running
  MariaDB/RQG pressure remains a separate objective.
- This adds evidence for expanding data pages, but broader DDL/file lifecycle
  recovery still needs durable file metadata and replay design.
