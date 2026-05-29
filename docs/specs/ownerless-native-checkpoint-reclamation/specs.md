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
  current first-party hooks expose `mylite_ownerless_innodb_current_lsn()` and
  `mylite_ownerless_innodb_checkpoint_lsn()` so tests can observe native
  current/checkpoint LSN ordering. `mylite_ownerless_innodb_make_checkpoint()`
  forces MariaDB's native `log_make_checkpoint()` from the runtime that owns
  `log_sys`. `mylite_ownerless_innodb_checkpoint_covers_lsn()` keeps the
  MariaDB-specific `FILE_CHECKPOINT` record coverage rule inside the InnoDB
  hook layer. These hooks do not by themselves prove that a checkpoint has
  covered a MyLite-selected page-version WAL boundary.
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

The first implementation slice adds an internal hook that reports native
checkpoint state, including current LSN and last checkpoint LSN, from the same
InnoDB runtime that owns `log_sys`. That hook is used only for diagnostics and
tests until a second slice proves a truncation rule.

The second implementation slice adds a conservative close-time reclamation
rule. When the last non-read-only runtime process is closing, MyLite first
checks that the process registry contains only the current live process. It
then forces a native InnoDB checkpoint and compares native
`last_checkpoint_lsn` with the durable MyLite page-visible LSN in
`mylite-concurrency.ckpt`. Recovery-open runtimes first advance local native
LSN state to that durable visible LSN before forcing the checkpoint, because a
retained page-version read can prove SQL visibility before local InnoDB redo
state has naturally caught up. Only when the native checkpoint covers the
durable visible LSN, including MariaDB's checkpoint-record margin where
applicable, does MyLite call
`mylite_ownerless_page_log_checkpoint_if_safe_at()` with that visible LSN. The
primitive truncates the page-version WAL only when every complete record is at
or below the safe LSN; otherwise the WAL remains retained. After a successful
all-record checkpoint, MyLite clears the shared page-version index under the
index latch.

Future live-peer or partial-record reclamation can call
`mylite_ownerless_page_log_checkpoint_at()` with a nonzero safe commit LSN only
when both MyLite and native evidence agree and reader-slot pressure is covered.

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
- Clean no-peer close coverage proving page-version WAL shrinkage after native
  checkpoint evidence, followed by ownerless and native exclusive reopen.
- Unsafe-hook crash coverage killing a process after native checkpoint proof but
  before page-log reclamation, proving retained WAL still recovers the committed
  update.
- Unsafe-hook race coverage pausing a close after native checkpoint proof,
  committing a newer peer update before the older closer resumes, and proving
  the newer records prevent unsafe truncation.
- Ownerless stress coverage proving committed rows remain visible through
  ownerless and native exclusive reopen after any retained-record truncation.
- `format-check` and `git diff --check`.

## Acceptance Criteria

- Native checkpoint evidence is exposed through internal hooks.
- No-peer close-time reclamation discards retained page-version records only
  after tests prove native checkpoint evidence at or beyond the durable visible
  LSN.
- A later reclamation implementation demonstrates bounded WAL shrinkage while
  live peers or retained newer records are present.

## Risks And Open Questions

- A clean native shutdown alone is not used as proof. Reclamation requires an
  explicit native checkpoint during the closing runtime and a comparison against
  the durable MyLite page-visible LSN.
- DDL/file lifecycle metadata is still incomplete, so reclamation must not
  depend on resolving dropped or renamed tablespaces from page-version records
  alone.
- Long ownerless readers can delay safe reclamation; reader-slot integration may
  need a busy/starvation policy before live-peer checkpointing is enabled.
