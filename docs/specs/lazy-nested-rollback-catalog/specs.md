# Lazy Nested Rollback Catalog

## Problem

Prepared row-DML execution opens a nested MyLite statement checkpoint for each
MariaDB statement savepoint. The nested checkpoint currently clones the parent
catalog root page eagerly even when the statement only mutates rows and commits.
That preserves rollback correctness, but it copies a full storage page on a hot
path where most prepared updates never read or mutate catalog metadata.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `packages/mylite-storage/src/storage.c` keeps two checkpoint views:
  `header` / `catalog_page` for the statement-start rollback point and
  `current_header` / `current_catalog_page` for the active view.
- `clone_parent_checkpoint_snapshot()` copies the parent header and currently
  copies the parent catalog page into the child rollback snapshot.
- Catalog metadata writers call `begin_write_journal(..., include_catalog=1)`
  before publishing catalog changes. Row storage mutations call the same helper
  with `include_catalog=0`.
- `mylite_storage_rollback_statement()` writes `statement->catalog_page` back
  to the statement-start catalog root and
  `collect_rollback_auto_increment_values()` consults that catalog snapshot
  when preserving auto-increment rollback state.
- `read_catalog_root()` already has a lazy current-catalog materialization path
  for nested checkpoints, but it still depends on the eager rollback catalog
  copy performed at nested begin.

## Design

- Add an explicit flag that records whether `statement->catalog_page` currently
  contains a valid rollback catalog snapshot.
- Top-level write and read checkpoint snapshots continue to materialize the
  rollback catalog eagerly because they read the durable checkpoint from disk.
- Nested checkpoints clone only the parent header at begin time and leave the
  rollback catalog snapshot unmaterialized.
- Materialize the nested rollback catalog snapshot when:
  - a catalog metadata writer enters `begin_write_journal(..., include_catalog=1)`;
  - rollback needs the statement-start catalog page; or
  - a nested catalog read wants to populate `current_catalog_page` from the
    statement-start view.
- The materializer copies an already-current matching catalog page when one is
  cached on the child or parent. Otherwise it reads and validates the catalog
  root named by the child's statement-start header before any child catalog
  write can modify it.

## Compatibility Impact

SQL, C API, and file-format behavior are unchanged. The slice preserves
statement/savepoint rollback semantics for row DML, catalog DDL, and
auto-increment rollback preservation while removing unnecessary work from
row-only nested checkpoint begin/commit cycles.

## Single-File And Lifecycle Impact

No new durable or transient files are introduced. The existing recovery and
transaction journal lifecycle remains unchanged. The lazy snapshot is purely
in-memory state owned by the active statement checkpoint.

## Tests And Verification

- Extend storage checkpoint coverage where necessary for nested catalog
  rollback and nested auto-increment rollback paths.
- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_statement_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`

## Acceptance Criteria

- Nested row-DML checkpoint begin does not copy the rollback catalog page.
- Nested catalog writers still capture the child begin catalog snapshot before
  publishing catalog changes.
- Rollback and auto-increment rollback preservation materialize the snapshot
  before reading or restoring it.
- Existing checkpoint, transaction, embedded statement, and routed storage
  tests pass.
- Prepared-update performance evidence is recorded before committing.

## Risks

- The helper must not materialize from a parent current catalog after the child
  has already changed catalog metadata. The `include_catalog=1` journal hook is
  therefore part of the correctness boundary.
- Future multi-page catalog work must revisit this helper so the lazily
  materialized rollback catalog covers the full catalog snapshot, not only the
  current single root page.
