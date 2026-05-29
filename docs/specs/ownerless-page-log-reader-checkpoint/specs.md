# Ownerless Page Log Reader Checkpoint

## Problem

Ownerless page-version readers scan `mylite-concurrency.wal` while checkpoint
or recovery code may rewrite or truncate complete records. The primitive already
uses a checkpoint read/write byte-range lock, but the test strategy still called
out long-reader/checkpoint coverage as a gap. Without deterministic evidence, a
future checkpoint change could compact the page-version WAL while a reader is
inside a stable read section.

## Source Findings

- MariaDB authority for ownerless storage remains the repository baseline
  `mariadb-11.8.6` (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), but this slice
  changes only first-party MyLite primitives and docs.
- `packages/libmylite/src/ownerless_page_log.cc` implements
  `mylite_ownerless_page_log_begin_read()` with a checkpoint read lock and
  `mylite_ownerless_page_log_checkpoint_at()` with a checkpoint write lock.
- The checkpoint path acquires the checkpoint write lock before the append lock,
  validates the page-log header, rewrites retained records, and truncates only
  after the protected rewrite path finishes.
- Ownerless SQL page-version reads call `mylite_ownerless_page_log_begin_read()`
  before resolving indexed or WAL-scanned page versions.

## Scope And Non-Goals

This slice adds deterministic primitive coverage for checkpoint waiting behind a
long reader. It does not change product checkpoint policy, WAL reclamation,
native redo/checkpoint reconciliation, SQL statement refresh behavior, public
API, directory layout, dependencies, or binary profile.

## Design

Add a cross-process primitive test:

- parent initializes a page-version log, appends one record, and enters a
  page-log read section;
- child opens the same log, signals that it is about to checkpoint, and calls
  `mylite_ownerless_page_log_checkpoint()` with a safe LSN that would truncate
  all records;
- parent waits briefly and asserts the child has not exited while the read
  section is held;
- parent releases the read section, waits for child success, and verifies the
  checkpoint truncation completed.

The test exercises real POSIX byte-range lock behavior across processes and
keeps the proof at the primitive layer where the checkpoint exclusion contract
lives.

## Compatibility Impact

No supported SQL or C API behavior changes. The compatibility claim is narrower:
the page-version log primitive now has direct evidence that active readers block
checkpoint truncation until the read section ends.

## Test Plan

- Run `mylite_ownerless_primitives_test` directly.
- Run embedded ownerless primitive CTest.
- Run ownerless unsafe-hook primitive CTest to prove the coverage also passes in
  the hook build.
- Run formatting and diff checks.

## Acceptance Criteria

- The child checkpoint remains blocked while the parent holds the page-log read
  lock.
- After the parent releases the read lock, the checkpoint completes and
  truncates the log to the fixed header.
- Existing ownerless primitive, SQL, hook, and stress coverage remains green as
  needed for the branch state.
