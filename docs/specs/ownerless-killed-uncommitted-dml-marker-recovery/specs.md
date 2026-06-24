# Ownerless Killed Uncommitted DML Marker Recovery

## Problem

Ownerless coverage now proves committed post-checkpoint DML survives a writer
that exits before `mylite_close()`. The complementary lifecycle is a writer that
has updated file-per-table pages inside an explicit transaction, then dies
before `COMMIT`. Concurrency is not complete if uncommitted page-version or
native file-operation evidence can be mistaken for a committed DML marker after
the owner dies.

## Source Findings

- MariaDB base: MariaDB 11.8.6 (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`),
  as recorded in `docs/architecture/engineering-standards.md`.
- `mariadb/storage/innobase/fil/fil0fil.cc::mtr_t::log_file_op()` can emit
  `FILE_MODIFY` redo for post-checkpoint file-per-table DML.
- `packages/libmylite/src/database.cc::
  mark_ownerless_native_file_op_checkpoint_after_successful_write()` persists
  the DML-only checkpoint marker after successful autocommit writes or explicit
  transaction commits that consumed process-local file-operation redo.
- Existing killed-writer coverage proves a dead uncommitted ownerless writer is
  cleaned up and its update is absent, but it uses the generic `ownerless_sql`
  table and does not force the post-checkpoint DML marker path.
- The committed killed-DML slice proves marker/WAL drain after `_exit(0)`
  following successful autocommit DML. This slice must prove the inverse:
  killing before commit leaves the marker clear and the pre-transaction row
  authoritative after no-live recovery.

## Design

- Add a focused ownerless SQL case that creates a file-per-table InnoDB table,
  inserts a payload row, forces an InnoDB checkpoint, clears the local file-op
  latch, then forks a child.
- The child opens ownerless read/write, starts an explicit transaction, updates
  the row, verifies no DML-only checkpoint marker has been persisted, signals
  the parent, and waits to be killed.
- The parent sends `SIGKILL`, waits for the child, verifies both native
  file-operation markers remain clear, opens ownerless read/write, and requires
  the pre-transaction row image.
- The recovering handle close must leave the DML marker clear and checkpoint
  any retained page-version WAL. Forced `.shm` rebuild and ordinary native
  reopen must still see the pre-transaction row and allow a later native write.

## Compatibility Impact

No SQL or C API behavior changes are intended. The slice adds deterministic
evidence that ownerless no-live recovery does not promote uncommitted
post-checkpoint DML into committed marker or page-version state.

## Directory And Native Storage Impact

No directory layout changes. The slice verifies existing process-registry
cleanup, InnoDB native recovery, DML marker, and page-version WAL handling for a
dead uncommitted ownerless writer.

## Non-Goals

- No arbitrary-instruction crash hook.
- No live-peer recovery claim for uncommitted DML marker state.
- No savepoint or broader DML-origin matrix in this slice.
- No SQL-level table-lock fault injection.

## Test Plan

- Add a direct selector:
  - `native-killed-uncommitted-dml-file-op-marker-recovery`
- Register the case in the ownerless SQL shard list.
- Run the direct selector, the affected CTest shard, adjacent DML marker
  selectors, production build guard, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- The child dies before `COMMIT` after post-checkpoint file-per-table DML.
- No generic native file-operation marker or DML-only marker is persisted for
  the uncommitted update.
- No-live ownerless recovery sees the pre-transaction row image.
- The recovering close leaves markers clear and page-version WAL checkpointed.
- Forced `.shm` rebuild and ordinary native reopen still see the
  pre-transaction row before a later native write.

## Risks

- This is process-death coverage after SQL execution reaches the waiting point,
  not arbitrary crash coverage at every instruction boundary.
