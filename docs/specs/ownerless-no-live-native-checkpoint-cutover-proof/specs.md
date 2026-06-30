# Ownerless No-Live Native Checkpoint Cutover Proof

## Problem

No-live ownerless close-time reclamation can compact page-version WAL only after
native InnoDB files are authoritative for the reclaimed page-visible LSN. The
existing `native-reclaim` SQL selector proved final visibility and empty-WAL
state, but it did not delete the checkpointed WAL before ordinary native reopen
or directly assert that InnoDB checkpoint state covers the reclaimed visible LSN.

This slice adds a focused cutover proof for a non-empty bulk insert path, which
is one of the hot ownerless paths identified by the production performance
probe.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` `mtr_t::commit()` writes
  mini-transaction redo for modified pages before releasing resources.
- `mariadb/storage/innobase/buf/buf0flu.cc` `log_make_checkpoint()` waits for
  dirty pages up to the current LSN and then advances the native FILE_CHECKPOINT
  record.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  `mylite_ownerless_innodb_checkpoint_covers_lsn()` treats the native checkpoint
  LSN, plus the FILE_CHECKPOINT record size, as coverage for a target LSN.
- `packages/libmylite/src/database.cc`
  `reclaim_ownerless_page_log_after_native_checkpoint()` advances no-live
  page-visible state, seeds shared redo state, requires native checkpoint proof,
  and checkpoints page-version WAL at the durable visible LSN.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  `native-reclaim` already covers normal no-live reclamation and native reopen,
  while unsafe-hook selectors cover crash before truncation and a newer-peer
  race.

## Design

Add one focused SQL harness selector:
`no-live-native-checkpoint-cutover-proof`.

The selector:

1. Creates an ownerless InnoDB table with one seed row so the later multi-row
   insert runs against a non-empty table.
2. Performs a 64-row ownerless `INSERT ... VALUES` through the existing compact
   multi-row helper.
3. Closes the final ownerless process, waits for page-version WAL compaction,
   and captures the durable checkpoint-visible LSN plus volatile redo-visible
   LSN.
4. Asserts the checkpoint-visible LSN advanced and the volatile redo state is at
   least as new as the durable checkpoint.
5. Deletes `mylite-concurrency.wal` and `mylite-concurrency.shm`.
6. Opens ordinary `MYLITE_OPEN_READWRITE`, asserts
   `mylite_ownerless_innodb_checkpoint_covers_lsn(reclaimed_visible_lsn)`, and
   verifies count, sum, and value predicates.

## Compatibility Impact

No public API, SQL behavior, or directory layout changes. The slice strengthens
evidence for an existing partial ownerless-concurrency claim: after no-live
payload-WAL reclamation, native files alone are sufficient for ordinary reopen
at the reclaimed page-visible LSN.

## Database Directory And Lifecycle Impact

The test intentionally deletes checkpointed `concurrency/mylite-concurrency.wal`
and transient `concurrency/mylite-concurrency.shm` after the final ownerless
close. Ordinary reopen must use native InnoDB files and `.ckpt` state rather
than page-version WAL overlay.

## Native Storage Impact

No native storage code changes are planned. The proof relies on existing
InnoDB dirty-page flush and checkpoint semantics, plus MyLite's current
no-live reclaim path.

## Non-Goals

- No changes to native undo, MTR commit, or redo semantics.
- No claim for arbitrary DDL/file-lifecycle cutover beyond the existing covered
  DDL classes.
- No SQL-level table-lock fault injection; prior investigation did not find a
  reachable ownerless table-wait callback shape from explored SQL.
- No external MariaDB/RQG stress expansion.

## Verification Plan

- Production embedded build of `mylite_ownerless_cross_process_sql_test`.
- Direct run of `no-live-native-checkpoint-cutover-proof`.
- Focused CTest for the new selector plus nearby native reclaim coverage.
- Existing ownerless stress/static gates appropriate for a test/docs slice.

## Acceptance Criteria

- The new selector passes in a production embedded build.
- It fails if retained page-version WAL is required for ordinary native reopen
  after no-live reclaim.
- The companion `duplicate-page-checkpoint-cutover` selector proves older
  retained same-page WAL records are discarded only after exact proof or a
  verified later same-page successor.
- Docs and compatibility matrix describe the stronger evidence while keeping
  broader redo/checkpoint, DDL/file-lifecycle, and external stress gaps partial.
