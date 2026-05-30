# Ownerless Active-Pin Reclaim

## Problem

Close-time ownerless page-version WAL reclamation can safely run with live peers
only when no page-version snapshot pins are active. That prevents incorrect
snapshot reads, but it also leaves a WAL-growth gap for long repeatable-read or
serializable transactions after the page-log layer has enough evidence to keep
their required boundary images.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- InnoDB stores page LSNs in page frames during mini-transaction commit in
  `mariadb/storage/innobase/mtr/mtr0mtr.cc`; MyLite page-version replay must
  preserve page images, not just SQL row-level metadata.
- MyLite publishes page-version visibility through
  `ownerless_innodb_pages_visible_hook()` in
  `packages/libmylite/src/database.cc` only after syncing the page-version WAL
  and then persists the visible LSN to `mylite-concurrency.ckpt`.
- Close-time reclaim is driven by
  `reclaim_ownerless_page_log_after_native_checkpoint()` in
  `packages/libmylite/src/database.cc`.
- The page-version pin registry in
  `packages/libmylite/src/ownerless_page_pin_registry.cc` can snapshot the
  active pin count and oldest pinned read LSN under its registry latch.
- `mylite_ownerless_page_log_checkpoint_preserving_oldest_snapshot_at()` in
  `packages/libmylite/src/ownerless_page_log.cc` scans the WAL and invokes its
  prepare callback only after proving every snapshot-sensitive data page
  advanced past the oldest pin has a retained boundary record at or below that
  pin.
- InnoDB page type constants in
  `mariadb/storage/innobase/include/fil0fil.h` identify B-tree, R-tree, BLOB,
  compressed-BLOB, legacy-unknown, instant-root, and compressed page families as
  page-version records that can hold snapshot-visible table/index payload.
  Undo, allocation, file-segment, tablespace-header, extent-descriptor, change
  buffer, transaction-system, and other system pages are native recovery/MVCC
  support state and do not need an oldest snapshot image before native
  checkpointing.

## Design

When live peers have active page-version pins, the product close path will call
the boundary-preserving page-log checkpoint primitive instead of returning
immediately. The primitive uses the durable page-visible checkpoint LSN as the
safe commit LSN and the oldest active page-version pin as the snapshot boundary.

Boundary proof applies conservatively to InnoDB data-page records: B-tree and
R-tree index pages, blob pages, compressed blob pages, unknown legacy pages,
instant-alter root pages, compressed pages, and any future or unrecognized page
type. Only explicit native support-state pages such as undo, allocation,
file-segment, tablespace-header, extent-descriptor, change-buffer,
transaction-system, and system pages bypass the oldest-snapshot boundary check
because older SQL snapshots need the latest durable undo/allocation support
state rather than an older physical image of those pages. After native
checkpoint proof, support-state records at or below the safe visible LSN are
dropped instead of retained as active-snapshot boundaries.

The native InnoDB checkpoint and page-index invalidation move into the
primitive's prepare callback for this path. That preserves the required order:
prove page-log boundary evidence first, then advance local InnoDB LSN state,
refresh external clean pages, force a native checkpoint, prove the checkpoint
covers the durable visible LSN, mark the page index for WAL-scan fallback, and
only then rewrite/truncate the page-version WAL.

If boundary proof is incomplete, the primitive returns busy before the prepare
callback runs. Product close treats that as a safe no-op: the WAL and page index
remain unchanged, and later closes can retry.

## Non-Goals

- The follow-on `ownerless-native-boundary-synthesis` slice can synthesize
  missing boundary records at page-version publish time when the older native
  page image is still readable and its page LSN is at or before the oldest
  active snapshot. If that proof is unavailable, active-pin reclaim still
  leaves the WAL unchanged.
- This slice does not add group commit or background checkpointing.
- This slice does not introduce user-visible checkpoint pressure diagnostics.
- This slice does not change snapshot isolation semantics or the public C API.

## Compatibility Impact

SQL compatibility is unchanged. Active repeatable-read and serializable
transactions still read from the page-version WAL when native files advance
past their read LSN. The change only reduces retained WAL records when the
required page-version boundary evidence exists.

## Directory And Lifecycle Impact

No new files or layout changes are introduced. Reclamation still operates only
on existing ownerless coordination files inside `concurrency/`: the WAL,
checkpoint metadata, and shared-memory page index.

## Native Storage Impact

The native checkpoint remains the proof that page-version records at or below
the durable visible LSN can be removed or reduced to boundary records. The
active-pin product path keeps newer snapshot-page records for later active
readers. With active pins, the native checkpoint is now delayed until after the
page-log scan proves older snapshots can still be reconstructed.

## Test Plan

- Keep existing SQL coverage proving a live snapshot pin with no page-log
  boundary blocks reclamation and preserves the reader snapshot.
- Add primitive coverage proving native support-state page records do not
  create a missing-boundary busy result, while an R-tree index page without an
  oldest-snapshot boundary still does.
- Add ownerless SQL coverage that:
  - holds an older repeatable-read snapshot so an initial writer leaves
    boundary records in the page-version WAL;
  - starts a second repeatable-read snapshot that can use those boundary
    records;
  - kills the older snapshot holder so only the boundary snapshot remains;
  - commits a newer update on only one of the logged tables;
  - proves close-time active-pin reclaim drops independent old records,
    preserves the boundary records the reader needs, and keeps newer records;
  - verifies normal reopen and forced `.shm` rebuild preserve the final data.
- Run embedded, ownerless hook, ownerless stress, format, and diff checks.

## Acceptance Criteria

- Active pins no longer unconditionally skip product page-log reclamation.
- Native checkpoint side effects do not run when page-log boundary proof is
  incomplete.
- Active-pin reclaim preserves the oldest snapshot boundary and newer
  snapshot-page records.
- Active-pin reclaim drops checkpointed support-state page records that do not
  require snapshot boundaries.
- Existing zero-active-pin live-peer reclamation and crash/race hook coverage
  continue to pass.

## Risks

- Holding the append lock while the active-pin prepare callback performs a
  native checkpoint is conservative and can block concurrent writers. Pressure
  policy and group-commit optimization remain separate work.
- Missing page-log boundary records still force a safe busy/no-op result when
  page-publish-time native boundary synthesis cannot prove an older native page
  image.
