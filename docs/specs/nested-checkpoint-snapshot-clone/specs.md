# Nested Checkpoint Snapshot Clone

## Problem

Prepared update loops run each statement inside a MyLite statement checkpoint
when an outer SQL transaction is active. Sampling after the page checksum
zero-tail slice shows a large share of prepared update time in nested checkpoint
startup: the child checkpoint reads the current active header page, encodes or
decodes checksums, and validates the catalog root even though the parent
checkpoint already owns an exclusive file lock and an in-memory validated
snapshot.

## Source Findings

- `packages/libmylite/src/database.cc::execute_statement()` opens a
  `StorageStatementCheckpoint` for prepared row DML inside an active
  transaction.
- `packages/mylite-storage/src/storage.c::begin_checkpoint()` receives the
  current active statement as `parent` for nested checkpoints.
- `initialize_checkpoint_statement()` already reuses the parent file handle,
  but `begin_checkpoint()` still calls `read_checkpoint_snapshot()` for the
  child.
- `read_page_at()` serves the active header page from the parent current header,
  which means nested startup can encode and then decode the same state without
  adding visibility or corruption evidence.

## Design

When a write checkpoint has a parent, initialize the child snapshot directly
from the parent:

- copy the parent current header as the child statement-start header;
- encode that header page for rollback publication;
- copy the parent current catalog page as the child statement-start catalog;
- initialize the child current header/catalog from the copied snapshot;
- keep top-level checkpoints on the existing file-read and validation path.

This preserves rollback semantics because a child rollback needs the exact
parent-visible checkpoint state at child start, not a revalidated copy of bytes
that the same parent would synthesize.

## Compatibility Impact

No SQL-visible behavior should change. The optimization is internal to nested
MyLite storage checkpoints under an already active owner.

## Single-File And Lifecycle Impact

No file-format or companion-file change. Top-level open, recovery, locking, and
corruption checks still read and validate persisted bytes.

## Test And Verification Plan

- Rely on existing storage checkpoint, nested rollback, transaction, and
  storage-engine savepoint coverage.
- Build storage-smoke targets.
- Run focused storage and embedded storage-engine smoke tests.
- Run the full storage-smoke CTest suite.
- Run the local performance baseline at small and higher iteration counts.
- Run `git diff --check`.

## Acceptance Criteria

- Nested checkpoints start from the parent current header/catalog snapshot.
- Nested rollback restores parent-visible rows, indexes, catalog metadata, and
  file length exactly as before.
- Top-level checkpoints still validate durable header/catalog pages.
- The local performance baseline reduces prepared row-DML overhead in an active
  transaction.

## Risks

- Copying the wrong parent snapshot would affect savepoint rollback. The change
  must use the parent current state, not the original parent-start state.
- This does not remove the remaining page-write syscall cost or the larger
  planned pager/WAL/index-navigation work.
