# Ownerless Partial Page-Log Reclamation

## Problem Statement

The no-peer native checkpoint reclamation path can safely truncate
`mylite-concurrency.wal` only when every complete page-version record is at or
below the durable visible LSN. If a peer opens after the closing process has
proved a native checkpoint and commits a newer record before reclamation, the
current path must leave all older records in place even though native recovery no
longer needs them.

This slice reclaims those older records while retaining newer records and
keeping the shared page-version index correct. It does not implement live-reader
reclamation for records that a pre-existing ownerless peer may still need.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant MyLite and MariaDB-derived source paths:

- `packages/libmylite/src/database.cc`
  - `reclaim_ownerless_page_log_after_native_checkpoint()` currently requires
    no live peers, reads the durable `.ckpt` visible LSN, advances local native
    InnoDB to that LSN, forces `mylite_ownerless_innodb_make_checkpoint()`,
    verifies `mylite_ownerless_innodb_checkpoint_covers_lsn()`, then calls
    `mylite_ownerless_page_log_checkpoint_if_safe_at()`.
  - `refresh_ownerless_external_pages_before_statement()` computes the
    thread-local page-version read LSN from the shared latest/visible LSNs and
    pins that LSN for repeatable-read and serializable explicit transactions.
    Those pins are currently in process-local `mylite_db` state, not a shared
    reclamation registry.
  - `ownerless_read_view_register_hook()` stores InnoDB transaction read-view
    fields in `ownerless_read_view_registry`, but it does not store
    page-version read LSNs.
- `packages/libmylite/src/ownerless_page_log.cc`
  - `mylite_ownerless_page_log_checkpoint_at()` holds the checkpoint write lock
    and append lock, rewrites records with `commit_lsn > safe_commit_lsn` to the
    front of the log, invokes the retained-record callback with new offsets, and
    finally truncates and syncs the log.
  - `mylite_ownerless_page_log_checkpoint_if_safe_at()` returns without
    truncation if it finds any complete record above the safe LSN.
  - page-log readers take the checkpoint read lock before using index offsets or
    scanning the WAL, so a checkpoint write lock excludes page-version reads
    while compaction is moving offsets.
- `packages/libmylite/src/ownerless_page_index.cc`
  - `mylite_ownerless_page_index_replace()` clears the index and republishes a
    retained-record set, clearing the WAL-scan-required flag on success.
  - `mylite_ownerless_page_index_require_wal_scan()` makes readers ignore
    indexed offsets and scan the WAL, which is the safe fallback if index
    replacement cannot complete.
  - `mylite_ownerless_page_index_publish()` currently treats an equal
    commit/page LSN as publishable. A writer that appended before compaction but
    publishes its old offset after compaction could overwrite the replacement
    offset unless same-version publishes are ignored.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  - `mylite_ownerless_innodb_read_page_version()` reads page-version records at
    the thread-local `page_visible_lsn`.
  - `mylite_ownerless_innodb_make_checkpoint()` wraps `log_make_checkpoint()`.
  - `mylite_ownerless_innodb_checkpoint_covers_lsn()` checks
    `log_sys.last_checkpoint_lsn` with the MariaDB checkpoint-record margin.

## Design

The close-time reclamation path will keep its existing no-live-peer proof and
native checkpoint proof, but after the proof it will perform partial page-log
checkpointing instead of all-or-nothing checkpointing:

1. Mark the page-version index as WAL-scan-required before moving any WAL
   offsets. This gives a safe fallback if a peer starts after the no-peer proof
   or if index replacement cannot finish.
2. Call a page-log checkpoint operation that keeps the checkpoint write lock and
   append lock until both WAL truncation and an optional completion callback
   finish.
3. Collect every retained record with its new offset through the existing
   retained-record callback.
4. After the WAL has been truncated and synced, but before the page-log locks
   are released, replace the page index from the retained-record set. An empty
   retained set clears the index.
5. Treat same commit/page-LSN page-index publishes as stale duplicates and leave
   the existing index entry unchanged. This prevents a writer that appended just
   before compaction from republishing its pre-compaction offset after the
   replacement has installed the new offset.

The WAL remains the authority if the page-index replacement fails. The
WAL-scan-required flag remains set in that case, so page-version reads scan the
compacted WAL instead of trusting stale offsets.

## Scope

In scope:

- Partial close-time reclamation after native checkpoint evidence.
- Retaining records above the durable visible LSN.
- Replacing or safely disabling the page-version index while offsets move.
- Deterministic tests for the newer-peer race and stale same-version index
  publish behavior.

Out of scope:

- Reclaiming records while a peer that existed before the checkpoint proof may
  have pinned an older page-version read LSN.
- A shared page-version read-LSN registry. The later
  `ownerless-live-reclaim-gating` slice adds a conservative registry gate that
  permits live-peer reclamation only when no active page-version pins exist;
  page-aware pruning while pins are active remains out of scope here.
- Dynamic/continuous background reclamation while the ownerless database remains
  busy.
- DDL/file-lifecycle metadata expansion beyond records already represented in
  the page-version WAL.

## Compatibility Impact

SQL semantics do not change. The slice only reduces retained
`mylite-concurrency.wal` bytes after the existing native checkpoint proof.
Readers still see MariaDB/InnoDB committed page versions through native pages or
through retained page-version WAL records.

Ownerless compatibility remains partial because live-reader reclamation still
requires a shared page-version read-LSN pinning design.

## Directory And Lifecycle Impact

All durable state remains in the MyLite database directory:

- `concurrency/mylite-concurrency.wal`
- `concurrency/mylite-concurrency.shm`
- `concurrency/mylite-concurrency.ckpt`

The lifecycle remains close-time opportunistic reclamation. A crash during
partial checkpointing leaves either the old WAL, a compacted WAL with a
WAL-scan-required index, or a compacted WAL with a replaced index; all states
must reopen from directory state.

## Native Storage Impact

Reclamation is allowed only after native InnoDB checkpoint evidence covers the
durable visible LSN. Records at or below that LSN may be reclaimed because the
native tablespace and redo state are sufficient for recovery. Records above that
LSN remain in the page-version WAL.

## Public API And Layout Impact

No public `libmylite` API changes. A first-party internal page-log helper gains
a checkpoint completion callback so close-time reclamation can update the page
index while page-log offsets are still protected by the checkpoint locks.

No shared-memory segment layout change is required.

## Binary Size Impact

The slice adds a small callback wrapper and retained-record vector in
`libmylite`. It does not add a dependency or a new MariaDB subsystem.

## Test Plan

- Extend primitive tests so page-log checkpointing can replace the page index
  before releasing checkpoint locks.
- Assert stale same-version page-index publishes do not overwrite an existing
  replacement offset.
- Extend the existing native-checkpoint race SQL test to assert that the WAL
  shrinks after a peer commits a newer record while the closer is paused after
  native checkpoint proof.
- Run embedded ownerless SQL tests, ownerless hook tests, ownerless stress, the
  primitive test target, `format-check`, and `git diff --check`.

## Acceptance Criteria

- Close-time reclamation compacts records at or below the durable visible LSN
  even when newer records appear after native checkpoint proof.
- Records above the safe LSN remain readable before and after forced `.shm`
  rebuild.
- The page index either points at retained records' new offsets or forces WAL
  scanning; it must not publish stale pre-compaction offsets for the same
  commit/page LSN.
- Existing crash and stress coverage remains green.

## Risks And Unresolved Questions

- This does not prove safe reclamation for live readers that pinned a
  pre-reclamation page-version read LSN. That requires a shared LSN pin registry
  or an equivalent source-backed proof.
- If page-index replacement fails under unexpected latch corruption, WAL scan
  fallback is safe but slower until shared-memory rebuild.
