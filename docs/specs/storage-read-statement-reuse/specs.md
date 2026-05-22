# Storage Read Statement Reuse

## Problem

Hot primary-key point reads spend more time opening and closing short storage
read statements than they spend in the exact-index lookup itself. On the local
storage-smoke benchmark before this slice:

- `storage-read-statements`: 4.138 us/op for begin/end pairs.
- `storage-pk-entry-lookups`: 4.203 us/op.
- `storage-pk-entry-lookups-one-read`: 0.186 us/op.
- `storage-pk-row-lookups`: 4.885 us/op.
- `storage-pk-row-lookups-one-read`: 0.635 us/op.
- `prepared-pk-selects`: 8.215 us/op.

The large gap between the per-lookup and held-read-scope storage phases shows
that read-statement setup dominates the storage side of point reads. The first
optimization should reduce per-scope overhead without changing the lock,
recovery, or visibility model.

## Source Findings

MariaDB handler point reads enter the MyLite handler through
`ha_mylite::index_read_map()` and the exact unique fast path in
`ha_mylite::read_exact_unique_index_row_into()`:

- `mariadb/storage/mylite/ha_mylite.cc`
- `ha_mylite::read_exact_unique_index_row_into()`
- `Mylite_read_statement_scope`

The storage layer opens short read statements with
`mylite_storage_begin_read_statement()` and closes them with
`mylite_storage_end_read_statement()`:

- `packages/mylite-storage/src/storage.c`
- `mylite_storage_begin_read_statement()`
- `initialize_read_statement()`
- `mylite_storage_end_read_statement()`

Current read-statement startup allocates a full `mylite_storage_statement`
object with `calloc()`, copies the filename, runs recovery checks, takes a
shared lock, reads page `0`, and either reuses or refreshes the decoded
checkpoint cache. Current shutdown closes or caches the file handle, clears
statement-owned caches, frees the filename, and frees the statement object.

Nested write checkpoints already retain one cleaned statement object per
thread with `reusable_nested_checkpoint_statement`. That provides a local
pattern for bounded statement-object reuse.

## Proposed Design

Add one thread-local reusable read-statement object:

- Allocate read statements through `allocate_read_statement()`.
- Mark read statements as eligible for reuse.
- On cleanup, clear all owned resources exactly as today, then retain one
  cleaned object in `reusable_read_statement`.
- Reinitialize retained statement storage before publishing it to the cache.
- Keep the existing shared-lock, journal-recovery, checkpoint-cache, file-cache,
  and filename-copy behavior unchanged.

Only the statement object storage is reused. The slice does not keep a read
lock open across SQL statements and does not weaken cross-process visibility or
crash-recovery checks.

## Affected Subsystems

- First-party storage implementation in `packages/mylite-storage/src/storage.c`.
- Storage unit tests in `packages/mylite-storage/tests/storage_test.c`.
- Local performance baseline documentation in `docs/ROADMAP.md`.

No MariaDB upstream-derived source changes are required.

## Compatibility Impact

No SQL, C API, metadata, or storage-engine routing behavior changes. The
opaque read-statement handle remains valid only until
`mylite_storage_end_read_statement()`. Reusing internal storage after cleanup
does not expose a new public lifetime guarantee.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Transient rollback and
transaction journals, file locks, and read snapshots keep the existing
lifecycle. The reusable object is process-local memory only.

## Public API And File Format Impact

No public API or file-format changes.

## Storage-Engine Routing Impact

Routed `InnoDB`, `MyISAM`, `Aria`, `MEMORY`, `HEAP`, and `BLACKHOLE` behavior is
unchanged. The optimization applies below routing to short durable read scopes.

## Wire-Protocol And Integration Impact

No wire-protocol or integration-package impact.

## Binary-Size Impact

Small code-size increase from one thread-local pointer and helper functions.
No new dependencies.

## Test And Verification Plan

- Extend storage tests to verify repeated read statements work after a cached
  statement object is retained and reused.
- Run `cmake --build --preset dev --target mylite_storage_test`.
- Run `ctest --preset dev --output-on-failure -R mylite-storage.capabilities`.
- Rebuild the storage-smoke MariaDB archive and relink affected smoke targets.
- Run focused storage-smoke tests for embedded storage behavior.
- Run local performance phases before and after the change:
  `storage-read-statements`, `storage-pk-entry-lookups`,
  `storage-pk-row-lookups`, and `prepared-pk-selects`.

## Acceptance Criteria

- Repeated begin/end read-statement loops remain correct.
- Existing storage read-statement session, checkpoint-cache, and file-cache path
  replacement tests pass.
- `storage-read-statements` improves or is at least neutral on the local
  benchmark.
- Point-read benchmarks do not regress meaningfully.
- No lock, recovery, or snapshot semantics are relaxed.

## Risks And Unresolved Questions

- The remaining dominant cost may still be journal probing, locking, and page-0
  reads. This slice intentionally avoids changing those semantics.
- Pointer reuse is internal. Tests should validate behavior and use test hooks
  only for the reuse cache, not add public API guarantees.
