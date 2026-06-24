# Ownerless Killed DML Marker Recovery

## Problem

The DML native checkpoint marker path now drains after no-live native proof
during orderly close and after final peer release. A remaining lifecycle class
is a process that successfully commits post-checkpoint DML, persists the
DML-only marker and page-version WAL, and then dies before `mylite_close()` can
run reclaim. Concurrency is not complete if that marker/WAL evidence is only
proven for orderly close paths.

## Source Findings

- MariaDB base: MariaDB 11.8.6 (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`),
  as recorded in `docs/architecture/engineering-standards.md`.
- `mariadb/storage/innobase/fil/fil0fil.cc::mtr_t::log_file_op()` logs
  InnoDB file-operation redo, including `FILE_MODIFY` records observed by the
  MyLite ownerless hook.
- `packages/libmylite/src/database.cc::
  mark_ownerless_native_file_op_checkpoint_after_successful_write()` consumes
  the process-local file-op redo latch after successful DML and persists the
  DML-only checkpoint marker.
- `reclaim_ownerless_page_log_after_native_checkpoint()` is normally reached
  during `mylite_close()`. A killed process skips that path, so the next opener
  must recover stale ownerless process state, preserve the committed page image,
  and only then drain marker/WAL evidence through the no-live native proof path.
- Existing `test_ownerless_zombie_writer_cleanup_before_reap()` proves a child
  can exit after a committed update and be cleaned before the parent reaps it,
  but it does not force the post-checkpoint file-per-table DML marker path.

## Design

- Add a focused ownerless SQL case that initializes a file-per-table InnoDB
  table with a payload column, forces an InnoDB checkpoint, clears the local
  file-op latch, then forks a child that runs an ownerless autocommit `UPDATE`
  and exits with `_exit(0)` without closing the database.
- The parent waits until the child is a zombie, verifies the DML marker is
  durable, opens the database in ownerless mode, verifies the committed row
  image, closes the handle, and requires the DML marker plus WAL to drain after
  no-live native proof.
- The test then forces `.shm` rebuild and verifies both ownerless and ordinary
  native reopen see the committed DML without retained WAL overlay.

## Compatibility Impact

No SQL or API behavior changes are intended. The slice adds deterministic
evidence for a killed committed-DML writer lifecycle.

## Directory And Native Storage Impact

No directory layout changes. The slice verifies that the existing
`concurrency/mylite-concurrency.ckpt` marker and page-version WAL are
sufficient recovery evidence when a writer dies before close-time reclaim.

## Non-Goals

- No new crash hook or unsafe test-only fault.
- No broad killed-transaction, savepoint, or multi-writer crash matrix.
- No change to generic dictionary/file-operation marker recovery.

## Test Plan

- Add a direct selector:
  - `native-killed-dml-file-op-marker-recovery`
- Register the case in the ownerless SQL shard list.
- Run the direct selector, the affected CTest shard, production build guard,
  `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- The child exits without `mylite_close()` after committed post-checkpoint DML.
- The parent observes a durable DML-only native checkpoint marker before
  recovery.
- Ownerless recovery sees the committed DML row image.
- After the recovering handle closes, no-live native proof clears the DML
  marker and checkpoints the page-version WAL.
- Forced `.shm` rebuild and ordinary native reopen still see the committed row.

## Risks

- A normal `_exit(0)` after SQL success is not a crash at an arbitrary CPU
  instruction. It is still a valuable process-lifecycle boundary because it
  skips MyLite close-time reclaim and process-slot cleanup.
