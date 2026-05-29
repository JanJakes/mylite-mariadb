# Ownerless Native Checkpoint Reclamation

## Goal

Define the source-backed boundary for safely reclaiming retained
`mylite-concurrency.wal` page-version records after no-live ownerless
tablespace replay. The end state is a bounded WAL retention policy that keeps
ownerless commits durable and visible through native exclusive reopen without
unbounded page-version WAL growth.

## Non-Goals

- Do not synthesize or rewrite native InnoDB checkpoint records without a
  MariaDB-source-backed hook.
- Do not discard page-version records only because they were copied to native
  tablespace files; native redo recovery can still replay an older local redo
  view afterward.
- Do not claim DDL/file-lifecycle page replay is complete; durable metadata for
  create, drop, rename, truncate, discard, and import remains separate work.
- Do not change public `libmylite` APIs or directory layout in the design
  slice.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`:
  `mtr_t::ownerless_redo_enter()` raises local InnoDB redo state from the
  ownerless latest LSN, `mtr_t::finish_writer()` reserves ownerless redo ranges
  before appending, and `mtr_t::ownerless_redo_leave()` reports completed redo
  writes before publishing the mini-transaction commit LSN.
- `mariadb/storage/innobase/buf/buf0flu.cc`:
  `log_t::write_checkpoint()`, `log_checkpoint_low()`,
  `log_checkpoint()`, and `log_make_checkpoint()` manage native redo
  checkpoint publication. Comments around `log_checkpoint_low()` note that
  `FILE_MODIFY` records are repeated after a checkpoint except on clean
  shutdown, where the log can be empty after the checkpoint.
- `mariadb/storage/innobase/log/log0recv.cc`:
  `recv_sys_t::find_checkpoint()` and
  `recv_recovery_from_checkpoint_start()` discover native redo checkpoints and
  start crash recovery from `log_sys.next_checkpoint_lsn`.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`:
  current first-party hook exports `mylite_ownerless_innodb_current_lsn()`, but
  not native `last_checkpoint_lsn` or a proof that a checkpoint has covered a
  MyLite-selected page-version WAL boundary.
- `packages/libmylite/src/database.cc`:
  `replay_concurrency_tablespaces()` applies visible page-version records to
  native tablespace files during no-live recovery, then calls
  `mylite_ownerless_page_log_checkpoint_at(..., 0U, ...)` so complete records
  are retained while corrupt or incomplete tails can still be trimmed.

## Compatibility Impact

The future implementation affects ownerless durability and performance rather
than SQL semantics. `docs/COMPATIBILITY.md` must stay partial until tests prove
that discarded page-version records are no longer needed for ownerless or
ordinary native exclusive reopen.

## Design

The safe reclamation boundary must combine MyLite and native InnoDB evidence:

- MyLite evidence:
  - retained page-version records have been replayed to native tablespace files,
  - every retained record at or below the reclamation LSN is represented by a
    current native file or explicitly belongs to a dropped tablespace,
  - no live ownerless reader can need the retained record for a pinned
    page-version read LSN.
- Native InnoDB evidence:
  - startup recovery has completed,
  - native dirty pages needed for the reclaimed records have been flushed,
  - native redo checkpoint state has advanced past the redo/history that could
    overwrite replayed page images with an older process-local view on the next
    startup.

The first implementation slice should add an internal hook that reports native
checkpoint state, including at least current LSN and last checkpoint LSN, from
the same InnoDB runtime that owns `log_sys`. It should only use that hook for
diagnostics and tests until a second slice proves a truncation rule. A later
truncation rule can call `mylite_ownerless_page_log_checkpoint_at()` with a
nonzero safe commit LSN only when both MyLite and native evidence agree.

## File Lifecycle

All retained and reclaimed state remains inside the MyLite database directory:

- `concurrency/mylite-concurrency.wal` is the page-version record source.
- `concurrency/mylite-concurrency.ckpt` remains the ownerless raw/page-visible
  LSN anchor.
- Native InnoDB redo, undo, and tablespace files remain under `datadir/`.

No durable state outside the MyLite database directory is allowed.

## Embedded Lifecycle And API

The native checkpoint evidence hook is internal to the embedded MariaDB
integration. It must not become a public `libmylite` API. Reclamation can run
only when the opener has exclusive/no-live ownership or when future reader-slot
logic proves no live ownerless reader can observe a reclaimed record.

## Build, Size, And Dependencies

The implementation is expected to add only small first-party hook code and
tests. It must not add dependencies. If MariaDB-derived source is edited, keep
the patch narrow and rebuild the embedded archive before verification.

## Test Plan

- Primitive tests for any new MyLite checkpoint/reclamation predicate.
- Embedded SQL coverage proving retained WAL is still needed before the native
  checkpoint proof is available.
- Unsafe-hook crash coverage around the first truncation point before any
  product truncation is enabled.
- Ownerless stress coverage proving committed rows remain visible through
  ownerless and native exclusive reopen after any retained-record truncation.
- `format-check` and `git diff --check`.

## Acceptance Criteria

- The first implementation exposes native checkpoint evidence without changing
  reclamation behavior.
- No product path discards retained page-version records before tests prove the
  native checkpoint boundary.
- A later reclamation implementation demonstrates bounded WAL shrinkage while
  preserving ownerless and native exclusive reopen correctness.

## Risks And Open Questions

- A clean native shutdown may checkpoint redo without proving every retained
  page-version image was read, dirtied, and flushed through InnoDB after MyLite
  replay. Treat clean shutdown alone as insufficient until source and tests
  prove otherwise.
- DDL/file lifecycle metadata is still incomplete, so reclamation must not
  depend on resolving dropped or renamed tablespaces from page-version records
  alone.
- Long ownerless readers can delay safe reclamation; reader-slot integration may
  need a busy/starvation policy before live-peer checkpointing is enabled.
