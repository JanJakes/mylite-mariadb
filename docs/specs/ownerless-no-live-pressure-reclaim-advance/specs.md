# Ownerless No-Live Pressure Reclaim Advance

## Problem

Ownerless active-reader pressure stress can leave
`concurrency/mylite-concurrency.wal` retained after the last snapshot reader
releases. The intended close-time path can reclaim records once no live peers
remain, but it currently checkpoints only to the durable page-visible LSN read
from `mylite-concurrency.ckpt`. If InnoDB has advanced raw redo past that
page-visible boundary before the final close-time reclaim, records newer than
the visible LSN are conservatively retained even though no process can still
hold an older ownerless page-version snapshot.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc` publishes ownerless modified page
  images and then calls
  `mylite_ownerless_innodb_flush_dirty_pages_to_lsn()` for the transaction
  commit LSN.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  exposes `mylite_ownerless_innodb_publish_buffer_pool_pages_to_lsn()` and
  `mylite_ownerless_innodb_flush_dirty_pages_to_lsn()` for publishing page
  images and durably advancing page-visible state.
- The same hook file reports raw redo progress through
  `mylite_ownerless_innodb_redo_state_leave()` and persists it separately from
  page-visible progress.
- `packages/libmylite/src/database.cc`
  `reclaim_ownerless_page_log_after_native_checkpoint()` reads both latest and
  visible LSNs from `mylite-concurrency.ckpt`, but the no-live reclaim branch
  uses only the visible LSN as the safe checkpoint boundary.
- Existing `active-reader-pressure` and `expanding-page-pressure` SQL selectors
  assert that WAL is retained while a repeatable-read page-version pin is live
  and checkpointed after the reader releases. Stress has shown the expanding
  selector can intermittently reach the final assertion with retained WAL.

## Design

Before no-live close-time reclaim, if the checkpoint file reports
`latest_lsn > visible_lsn`, advance the page-visible boundary to `latest_lsn`
inside the final ownerless runtime:

1. Publish currently buffered native page images at `latest_lsn`.
2. Flush dirty native pages through the existing ownerless InnoDB hook, which
   also persists the page-visible checkpoint on success.
3. Re-read `mylite-concurrency.ckpt`.
4. If shared redo publication still cannot advance page-visible state because
   the contiguous-written LSN is behind a native checkpoint-record gap, force a
   native checkpoint and persist `latest_lsn` as page-visible only after
   `mylite_ownerless_innodb_checkpoint_covers_lsn(latest_lsn)` succeeds.
5. Continue normal native checkpoint proof and page-version WAL checkpointing
   only up to the re-read visible LSN.

This advance is limited to the no-live-peer path. Live-peer reclaim keeps the
existing active-pin rules because advancing a visible boundary while another
process owns an active snapshot would change the retention problem and belongs
to a separate background checkpoint policy.

The implementation remains conservative: if publish, flush, or checkpoint
publication cannot advance visible state, the existing safe-retention behavior
remains in place and the WAL is not forcibly truncated.

## Scope And Non-Goals

In scope:

- No-live close-time page-visible advancement before page-version WAL reclaim.
- Focused expanding-page pressure stress evidence for the final reader-release
  checkpoint boundary.
- Documentation updates that keep broader pressure scheduling marked planned.

Out of scope:

- Background checkpoint workers or asynchronous pressure scheduling.
- Live-peer active-pin advancement beyond existing boundary-preserving reclaim.
- New page-version WAL record formats or native InnoDB file format changes.
- External MariaDB/RQG pressure oracles.

## Compatibility Impact

SQL semantics are unchanged. The slice only tightens internal ownerless
directory lifecycle behavior after the final ownerless process closes: committed
native state remains durable and the ownerless page-version WAL is less likely
to stay retained after no live snapshot can need it.

## Directory And Lifecycle Impact

No directory layout changes. The change uses existing
`concurrency/mylite-concurrency.wal`, `.ckpt`, and `.shm` files. Reclaim still
requires the final ownerless runtime to hold its process slot and checkpoint
locks before compacting the WAL.

## Native Storage Impact

No storage format changes. The slice relies on existing InnoDB buffer-pool page
publication, dirty-page flush, and native checkpoint proof. It may do extra
foreground work on final ownerless close when raw redo has advanced beyond the
durable page-visible LSN, but only on the no-live path where no peer needs
writer progress.

## Public API Impact

No public API changes.

## Binary Size Impact

The implementation adds a small first-party helper and no dependencies.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `no-live-pressure-reclaim` in `embedded-dev`.
- Run focused `expanding-page-pressure` in `embedded-dev`.
- Run focused `active-reader-pressure` in `embedded-dev`.
- Build and run the focused selectors in `ownerless-test-hooks`.
- Run full embedded and hook ownerless SQL CTest coverage.
- Run the ownerless pressure stress selectors and full `ownerless-stress`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Releasing the final repeatable-read snapshot reader leaves no page-version
  WAL records in the focused active-reader and expanding-page pressure tests.
- Ownerless and native exclusive reopen checks still preserve final row data
  before and after forced `.shm` rebuild.
- If page-visible advancement cannot prove a newer boundary, the implementation
  retains WAL instead of truncating unproven records.
- Compatibility docs continue to mark background checkpoint scheduling and
  external pressure oracles as planned.

## Risks And Follow-Up

- Publishing all current buffer-pool pages at no-live close can add close-time
  work on large databases; background checkpoint scheduling remains the right
  follow-up for applications that need smoother pressure behavior while peers
  remain live.
- This does not solve broader DDL/file-lifecycle recovery or external
  randomized stress gaps.
