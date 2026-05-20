# Nested Checkpoint Narrow Init

## Problem

Prepared row-DML inside an explicit transaction opens a nested MyLite statement
checkpoint for every execution. After nested filename aliasing, sampling still
shows `begin_checkpoint()` spending a large share of nested startup in
`calloc()`, mostly zeroing the full `mylite_storage_statement` object. That
object contains page-sized header and catalog buffers, but a nested write
checkpoint immediately overwrites the catalog snapshots from its parent and
does not need a materialized rollback header page unless rollback happens.

## Source Findings

- Target base: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The relevant code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`.
- `begin_checkpoint()` currently uses `calloc(1U, sizeof(*statement))` for both
  top-level and nested write checkpoints.
- `initialize_checkpoint_statement()` handles nested checkpoints by reusing the
  parent file handle and calling `clone_parent_checkpoint_snapshot()`.
- `clone_parent_checkpoint_snapshot()` writes the child header fields,
  `catalog_page`, `current_catalog_page`, catalog generation fields, and
  validity flags.
- `materialize_statement_header_page()` lazily encodes `header_page` for
  rollback, so nested commit does not require `header_page` to be zeroed.
- `free_statement()` only needs dynamic cache/list/buffer fields to be either
  valid allocations or null/zero values.

## Design

Add a narrow nested checkpoint allocator with one thread-local reusable nested
statement object.

- Top-level write checkpoints and read statements keep the existing `calloc()`
  path because they read from durable bytes and may use all page buffers.
- Nested write checkpoints use `malloc()` plus explicit scalar and cache/list
  initialization.
- After nested commit or rollback cleanup, one child checkpoint object can be
  retained for reuse by a later nested checkpoint on the same thread.
- The initializer zeroes every dynamic owner field that cleanup may free:
  exact-index caches, live-row caches, row-id caches, row-payload caches,
  table-entry cache, append-page buffer, buffered page undos, and buffered
  rewrite cache.
- The initializer does not clear `header_page`, `catalog_page`, or
  `current_catalog_page`. The nested snapshot clone overwrites the catalog
  pages, and `header_page` remains invalid until rollback materializes it.

This keeps rollback semantics unchanged while avoiding repeated clearing of
large unused page buffers and repeated statement object allocation/free on the
hot nested-commit path.

## Compatibility Impact

No SQL or C API behavior changes. This only changes internal initialization for
nested storage checkpoints.

## DDL Metadata Routing Impact

No metadata routing behavior changes. Nested DDL rollback still receives the
same cloned parent catalog snapshot before any metadata mutation.

## Single-File And Lifecycle Impact

No file-format or companion-file change. Top-level recovery, locking, durable
snapshot validation, and journal lifecycle are unchanged.

## Public API Or File-Format Impact

No public API or file-format impact.

## Storage-Engine Routing Impact

No routing behavior changes. The affected path is statement checkpoint lifetime
management used by routed durable row-DML and DDL rollback.

## Wire-Protocol Or Integration-Package Impact

None.

## Binary-Size Impact

Negligible: one small initializer helper, one thread-local pointer, and a
branch in checkpoint allocation and cleanup.

## License Or Dependency Impact

None.

## Test And Verification Plan

- Build storage-smoke targets.
- Run focused storage and embedded statement/storage-engine tests.
- Run the full storage-smoke CTest suite.
- Run `git diff --check` and `git clang-format --diff`.
- Run the prepared-update performance baseline and sample the hot path.

## Acceptance Criteria

- Nested write checkpoints no longer call `calloc()` for the full statement
  object and can reuse one cleaned-up nested statement allocation.
- Nested rollback still materializes and writes the statement-start header and
  catalog snapshot correctly.
- Cleanup remains safe on nested commit, rollback, initialization failure, and
  nested-savepoint depth greater than one.
- Focused and full storage-smoke tests pass.
- Prepared-update performance is neutral or improved.

## Risks And Unresolved Questions

- Missing a dynamic owner field in the explicit initializer would make
  `free_statement()` unsafe. The implementation must list every dynamic field
  from `mylite_storage_statement` rather than relying on partial structure
  assignment.
- The reusable object intentionally keeps at most one nested statement per
  thread. Deeper nested savepoint stacks still allocate additional objects, but
  repeated prepared DML uses the cached one.
- This still leaves the parent catalog snapshot copies in the hot path. A
  separate lazy catalog snapshot slice should handle that if profiling keeps
  showing catalog `memmove()` as material.
