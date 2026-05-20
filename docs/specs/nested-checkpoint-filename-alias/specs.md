# Nested Checkpoint Filename Alias

## Problem

Prepared row-DML loops inside an explicit transaction open a MyLite nested
statement checkpoint for each statement. After the prepared SQL policy cache
work, sampling still shows nested checkpoint startup on the hot path, including
per-statement filename allocation and copying even though a nested checkpoint
always refers to the same active parent file.

## Source Findings

- Target base: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The relevant code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`.
- `begin_checkpoint()` resolves the active parent checkpoint with
  `active_statement_for(filename)`, rejects unrelated active owners, allocates a
  new `mylite_storage_statement`, and copies `filename` for every child.
- `initialize_checkpoint_statement()` reuses the parent `FILE *` for nested
  checkpoints and clones only the current parent storage snapshot.
- `free_statement()` unconditionally frees `statement->filename`, so nested
  checkpoints cannot safely alias the parent filename without explicit
  ownership metadata.
- `packages/libmylite/src/database.cc::StorageStatementCheckpoint` opens these
  nested checkpoints around prepared row-DML executed inside active SQL
  transactions.

## Design

Track filename ownership on `mylite_storage_statement`.

- Top-level write and read checkpoints keep copying `filename` and own the
  allocation.
- Nested write checkpoints alias `parent->filename` and mark the filename as
  borrowed.
- `free_statement()` releases only owned filenames.
- All existing filename comparisons and journal paths continue to receive a
  stable null-terminated filename pointer.

The parent checkpoint is still on the active stack until the child commits or
rolls back, so the borrowed pointer lifetime covers all child cleanup paths.

## Compatibility Impact

No SQL, C API, or storage format behavior changes. This only removes redundant
allocation and copy work from nested MyLite storage checkpoint startup.

## DDL Metadata Routing Impact

No metadata routing behavior changes. Nested DDL rollback still uses the
existing cloned header and catalog snapshots.

## Single-File And Lifecycle Impact

No file-format or companion-file change. Top-level checkpoints still own their
filename strings, journals still derive paths from the same filename value, and
nested checkpoints keep borrowing only while the parent is active.

## Public API Or File-Format Impact

No public API or file-format impact.

## Storage-Engine Routing Impact

No routing behavior changes. The affected path is storage checkpoint lifetime
management used by routed durable row-DML and DDL rollback.

## Wire-Protocol Or Integration-Package Impact

None.

## Binary-Size Impact

Negligible: one integer field and a branch in statement cleanup.

## License Or Dependency Impact

None.

## Test And Verification Plan

- Build the storage-smoke targets that cover `mylite-storage` and embedded
  routed storage.
- Run the focused storage and embedded storage-engine tests.
- Run the full storage-smoke CTest suite.
- Run `git diff --check` and `git clang-format --diff`.
- Run the local prepared-update performance baseline to confirm the hot path is
  not regressed.

## Acceptance Criteria

- Nested write checkpoints no longer allocate or free their own filename copy.
- Top-level statements and read statements continue to own and free their
  filename copies.
- Nested commit and rollback coverage passes, including journal cleanup and
  savepoint rollback paths.
- The prepared-update performance baseline is neutral or improved.

## Risks And Unresolved Questions

- The borrowed filename must never outlive its parent checkpoint. The active
  statement stack already enforces LIFO commit/rollback, and the implementation
  should preserve that invariant.
- This is intentionally narrow. Larger checkpoint startup costs remain in
  statement allocation and snapshot copying.
