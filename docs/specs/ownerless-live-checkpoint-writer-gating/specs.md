# Ownerless Live Checkpoint Writer Gating

## Problem

Ownerless close-time page-version WAL reclamation can run while another
ownerless process remains open when the shared page-version pin registry reports
that no snapshot reader needs retained WAL records, or when active-pin boundary
proof is available. That is sufficient for page-version reader safety, but it is
not sufficient native redo safety: forcing a process-local InnoDB checkpoint
while another process still owns write, redo, lock, page-write, or dictionary
state can race MariaDB's native redo/checkpoint files.

Recent full-suite ownerless runs exposed the risk as intermittent native InnoDB
startup failures such as invalid log-header checksum or missing
`FILE_CHECKPOINT` records in selectors that pass in isolation. The next bounded
fix is to require live-peer native checkpoint reclamation to prove the shared
ownerless write/recovery state is idle before calling the native checkpoint
hook.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/buf/buf0flu.cc`
  `log_checkpoint_low()` writes native checkpoint metadata and documents that
  redo apply expects a `FILE_CHECKPOINT` record after the checkpoint except on a
  clean shutdown.
- `mariadb/storage/innobase/log/log0recv.cc`
  `recv_sys_t::find_checkpoint()` discovers the native checkpoint, and redo
  scan reports missing `FILE_CHECKPOINT` as log corruption during startup.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  `mylite_ownerless_innodb_make_checkpoint()` calls MariaDB
  `log_make_checkpoint()` in the current embedded runtime, while
  `advance_external_lsn()` can raise local redo state to a shared ownerless LSN
  before refresh/checkpoint work.
- `packages/libmylite/src/database.cc`
  `reclaim_ownerless_page_log_after_native_checkpoint()` already gates live
  reclamation on page-version pin state. The live path did not also require
  shared transaction, InnoDB lock, page-write, dictionary, and redo-state
  quiescence before forcing the native checkpoint.

## Scope And Non-Goals

In scope:

- Add a live-peer reclamation guard that rejects native checkpoint reclamation
  while shared ownerless write/recovery state is active.
- Keep no-live close-time reclamation unchanged.
- Keep live idle-peer and active-reader boundary reclamation available when
  native write/recovery state is idle.
- Add SQL coverage with an active live writer proving WAL is retained until the
  writer finishes.

Out of scope:

- Redesign native InnoDB redo files for true multi-process group checkpointing.
- Synthesize native redo checkpoint records.
- Change public `libmylite` APIs or directory layout.
- Add background checkpoint scheduling.

## Design

Before live-peer page-version WAL reclamation calls the native checkpoint
prepare hook, MyLite first takes a nonblocking ownerless statement gate that
conflicts with new dictionary DDL and write statements. If that gate is busy,
reclamation is skipped. While holding the gate, MyLite reads the mapped
ownerless shared-memory segments and requires:

- no active shared ownerless transaction entries,
- no active or waiting shared InnoDB lock entries,
- no active or waiting page-write lock entries,
- idle ownerless dictionary generation state,
- idle ownerless redo state with no active entries, reservations, or held redo
  latches.

Read-only snapshot state and page-version pins are not treated as native write
state. Those are still handled by the existing active-pin boundary proof.

If any write/recovery state is active while another process is live, or if a
write/DDL statement is already in progress, close-time reclamation leaves the
page-version WAL intact. A later no-live closer or a live-peer closer that
observes idle write/recovery state may reclaim under the existing native
checkpoint proof.

## Compatibility Impact

This changes no SQL semantics. It narrows a performance optimization: live-peer
WAL reclamation can be skipped while another ownerless writer is active. That
keeps ownerless recovery authority in `concurrency/mylite-concurrency.wal`
instead of forcing a native checkpoint during an unsafe shared native redo
window.

## Database Directory And Lifecycle Impact

All state remains in the existing MyLite-owned database directory:

- shared state is read from `concurrency/mylite-concurrency.shm`,
- retained page versions remain in `concurrency/mylite-concurrency.wal`,
- durable LSN anchors remain in `concurrency/mylite-concurrency.ckpt`.

No file names or public directory layout change.

## Test Plan

- Focused embedded selector:
  `./build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test live-reclaim`.
- Focused hook selector:
  `./build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test live-reclaim`.
- Embedded and hook ownerless SQL CTest subsets.
- `ownerless-stress` if the focused and subset checks pass.
- `format-check`, `git diff --check`, and cached diff check.

## Acceptance Criteria

- A live idle peer can still allow page-version WAL reclamation.
- A live active writer prevents peer close-time native checkpoint reclamation,
  leaving WAL retained while the writer is active.
- After the writer commits and closes, later no-live reclamation checkpoints the
  retained WAL and ownerless/native reopen read the final committed rows.
- Existing active snapshot-pin reclamation behavior remains covered.

## Risks And Open Questions

- This is intentionally conservative and can retain WAL longer under write
  pressure. The active-reader pressure limit and diagnostics remain the
  user-visible controls until background checkpoint scheduling is designed.
- Broader native redo/checkpoint reconciliation remains partial; this slice
  prevents one unsafe live checkpoint window but does not make native redo files
  fully process-shared.
