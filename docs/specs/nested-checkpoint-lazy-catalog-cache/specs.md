# Nested Checkpoint Lazy Catalog Cache

## Problem

Prepared row-DML inside an explicit transaction opens a nested MyLite statement
checkpoint for every execution. After nested checkpoint filename aliasing and
narrow initialization, sampling still shows nested startup spending measurable
time copying the parent catalog snapshot twice: once for rollback and once for
the child statement's current catalog cache. Ordinary row-DML already resolves
table metadata from the active table-entry cache, so the current catalog cache
is often never read before the nested checkpoint commits.

## Source Findings

- Target base: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`.
- `clone_parent_checkpoint_snapshot()` copies the parent catalog page into both
  `catalog_page` and `current_catalog_page` for every nested checkpoint.
- `mylite_storage_rollback_statement()` restores `catalog_page`, so the
  rollback baseline must remain eagerly available.
- `read_catalog_root()` is the normal catalog-read entry point and already
  populates `current_catalog_page` after a durable read.
- Catalog writes call `clear_statement_chain_catalog_root_caches()`, which
  clears `has_current_catalog_page`, root/generation guards, and table-entry
  caches down the active statement chain.

## Design

Nested write checkpoints should copy only the rollback catalog snapshot at
begin. They keep the current catalog root and generation guards, but mark
`current_catalog_page` as not materialized.

When `read_catalog_root()` sees an active nested checkpoint with an unmaterialized
current catalog cache and matching root/generation, it materializes
`current_catalog_page` from `catalog_page` and returns that page. This preserves
the existing invariant that a successful `read_catalog_root()` leaves the active
statement's current catalog cache available, while avoiding the second page copy
on row-DML statements that never read catalog metadata.

Catalog mutations continue to invalidate the lazy cache through the existing
statement-chain invalidation. After invalidation, later catalog reads fall back
to the normal buffered/durable page read path and repopulate the current cache
from the mutated catalog page.

## Compatibility Impact

No SQL or C API behavior changes. The change only defers an internal in-memory
catalog cache copy for nested storage checkpoints.

## DDL Metadata Routing Impact

DDL metadata routing semantics are unchanged. Nested DDL still has an eager
statement-start `catalog_page` rollback baseline, and catalog writes still
invalidate active catalog and table-entry caches before publication.

## Single-File And Lifecycle Impact

No file-format, journal, locking, or companion-file changes. The primary
`.mylite` file lifecycle remains unchanged.

## Public API Or File-Format Impact

None.

## Storage-Engine Routing Impact

No routing behavior changes. The affected path is checkpoint lifetime management
used by routed durable row-DML and DDL rollback.

## Wire-Protocol Or Integration-Package Impact

None.

## Binary-Size Impact

Negligible: one small lazy materialization helper and one branch in
`read_catalog_root()`.

## License Or Dependency Impact

None.

## Test And Verification Plan

- Add storage coverage for a nested statement reading parent-uncommitted catalog
  metadata before it mutates catalog metadata.
- Build storage-smoke targets.
- Run focused storage and embedded statement/storage-engine tests.
- Run full storage-smoke CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run the prepared-update performance baseline and inspect the sampled hot path.

## Acceptance Criteria

- Nested write checkpoints no longer eagerly copy the parent catalog snapshot
  into `current_catalog_page`.
- Nested rollback still restores statement-start catalog metadata.
- Nested statements can read parent-visible catalog metadata before and after
  their own catalog mutations.
- Existing row-DML, savepoint, DDL rollback, and embedded storage-engine tests
  pass.
- Prepared-update performance is neutral or improved.

## Risks And Unresolved Questions

- `catalog_page` and `current_catalog_page` intentionally have different roles:
  rollback baseline versus current read cache. The implementation must not defer
  the rollback baseline copy.
- If a parent checkpoint ever has a stale current-catalog cache while exposing a
  newer catalog root/generation, this change would preserve the existing
  behavior rather than fixing it. That should be investigated separately if
  profiling or tests expose it.
