# Ownerless Dead Snapshot Pin Reclaim

## Problem Statement

Ownerless page-log reclamation is intentionally blocked while a live
repeatable-read or consistent-snapshot transaction has a page-version pin. That
prevents prefix compaction from removing page-version WAL records an old reader
may still need. A killed reader is different: its process slot should be
cleaned by the next ownerless opener, its page-version pin should be released,
and live-peer close-time reclamation should be allowed once no active pins
remain.

This slice adds SQL-level coverage for that dead-reader path. It does not
implement page-aware active-reader pruning; live active pins still block prefix
compaction.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant MyLite source paths:

- `packages/libmylite/src/database.cc`
  - `ensure_ownerless_transaction_page_version_pin()` publishes a
    page-version pin for repeatable-read and consistent-snapshot ownerless
    transactions.
  - `release_ownerless_transaction_page_version_pin()` releases that pin on
    normal transaction end, rollback, close, or failed pin replacement.
  - `allocate_concurrency_process_slot()` calls
    `mylite_ownerless_process_registry_cleanup_dead_with_callback()` before
    allocating the current opener's process slot.
  - `ownerless_process_owner_state_requires_recovery()` treats active
    transactions, InnoDB locks, redo state, page-write locks, and dictionary
    state as recovery-sensitive. Dead read-only snapshot readers publish MDL,
    read views, and page-version pins, but those entries are cleanup state
    rather than write-recovery evidence when no hard recovery state remains.
  - `ownerless_process_cleanup_owner_state()` releases dead-owner
    page-version pins through
    `mylite_ownerless_page_pin_registry_release_owner()`.
  - `ownerless_runtime_can_reclaim_page_log()` allows live-peer reclamation
    only when the current runtime is the only live peer or when the shared
    page-version pin registry reports zero active pins.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - Existing SQL coverage proves a live repeatable-read snapshot pin blocks
    page-log reclamation until the reader commits and exits.
  - Existing primitive coverage proves page-pin owner cleanup, but there is no
    SQL test that kills a pinned reader while another ownerless peer remains
    live and then requires close-time reclamation to proceed.

## Design

Add a normal ownerless SQL test:

1. Initialize an ownerless database.
2. Keep one ownerless peer open so cleanup and reclamation happen with live
   peer state still present.
3. Start a second ownerless process in repeatable-read with a consistent
   snapshot and a page-version pin.
4. Commit a writer update and verify the retained page-version WAL is not
   reclaimed while the pinned reader is alive.
5. Kill the pinned reader.
6. Open and close another ownerless read/write handle. Its process-slot
   allocation must clean the dead reader, release its MDL, read view, and
   page-version pin, and allow close-time page-log reclamation despite the
   other live peer.
7. Verify the committed update remains visible through ownerless reopen and
   after forced `.shm` rebuild/native read-write reopen.

## Scope

In scope:

- Normal-build SQL evidence for dead page-version pin cleanup.
- Reclamation proof with another live ownerless peer still attached.
- Compatibility/spec updates that distinguish dead-pin cleanup from future
  page-aware pruning.

Out of scope:

- Reclaiming records while an active reader still needs older snapshots.
- Page-aware per-record or per-page pruning.
- New shared-memory layout, public API, or MariaDB hook changes.

## Compatibility Impact

No public SQL or C API behavior changes. The slice strengthens ownerless
concurrency evidence by proving killed snapshot readers do not permanently
starve page-version WAL reclamation.

## Directory And Lifecycle Impact

No directory layout changes. The test exercises existing
`concurrency/mylite-concurrency.shm`, `mylite-concurrency.wal`, and
`mylite-concurrency.ckpt` lifecycle behavior under live-peer cleanup.

## Native Storage Impact

The committed update remains stored through the existing page-version WAL plus
native checkpoint bridge. The test does not change InnoDB file formats or
recovery rules.

## Binary Size Impact

Test-only coverage. No production dependency, public symbol, or default build
profile change.

## Test Plan

- Build and run the ownerless cross-process SQL test selector for live
  reclamation.
- Run the normal embedded ownerless cross-process SQL CTest filter.
- Run the ownerless hook preset filter for ownerless cross-process SQL and
  negative proof.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- A live snapshot pin still blocks page-log reclamation.
- Killing the pinned reader does not lose the committed writer update.
- The next ownerless opener cleans the dead reader's MDL, read view, and
  page-version pin.
- Close-time page-log reclamation succeeds while another live ownerless peer
  remains open and no pins remain.
- Forced `.shm` rebuild/native read-write reopen sees the committed update.

## Risks And Open Questions

- Page-aware active-reader pruning remains future work; this slice only proves
  dead pins do not keep reclamation blocked forever.
