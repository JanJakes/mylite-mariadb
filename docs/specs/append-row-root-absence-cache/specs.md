# Append Row Root Absence Cache

## Problem Statement

Prepared inserts still spend most component time in `mylite_step()`. After
inline inserts encode directly into the active append buffer, the storage append
path still reloads the catalog image and scans table/index-root metadata for
each inserted row, even in an active transaction where the same table has
already been resolved and has no catalog-backed maintained index roots.

Updates already avoid this repeated catalog work through active table-entry and
table-index-root absence caches. Inserts should use the same narrower cache
pattern instead of reparsing catalog metadata on every row.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()` calls
  `mylite_storage_append_row_with_index_entries()` for inserted rows after
  preparing MyLite index-entry payloads.
- `packages/mylite-storage/src/storage.c::mylite_storage_append_row_with_index_entries()`
  currently reads the header, reads the full catalog image, finds the table
  record, predicts the row page id, and calls
  `plan_maintained_index_root_inserts()` for every insert.
- `packages/mylite-storage/src/storage.c::find_table_id_in_statement()` already
  resolves table ids through the same outer active table-entry cache used by
  update/delete paths.
- `packages/mylite-storage/src/storage.c::table_index_roots_absent_in_statement()`
  and `store_table_index_roots_absent_in_statement()` already cache that a
  table has no catalog index-root records for a given catalog root and
  generation.
- `packages/mylite-storage/src/storage.c::mylite_storage_update_row_with_index_entry_changes()`
  uses those caches before deciding whether to read the catalog and plan
  maintained-root updates.

## Scope

- Rework `mylite_storage_append_row_with_index_entries()` to resolve `table_id`
  through the active table-entry cache when possible and populate that cache
  from the catalog when needed.
- Skip catalog loading and maintained-root planning when the active cache says
  the table has no index roots.
- When catalog planning runs and finds no index roots for the table, store the
  absence marker for later inserts in the same active checkpoint.
- Preserve the existing catalog-read path when no active cache is available or
  when maintained index roots may exist.

## Non-Goals

- No catalog file-format changes.
- No broader catalog image view or table-entry cache rewrite.
- No change to maintained-root, branch-root, or append-tail index semantics.
- No SQL-visible behavior, public API, or storage-engine routing change.

## Design

Use the same high-level flow already used by the update path:

1. Resolve the current header.
2. Resolve `table_id` through the active table-entry cache when available.
3. Check the active table-index-root absence cache.
4. Read the catalog image and run maintained-root insert planning only when
   index entries exist and absence is not cached.
5. Cache table-index-root absence after a catalog read proves there are no
   index-root records for the table.

If maintained-root planning is skipped, the initialized insert plan leaves
`index_entry_changed == NULL`, which the existing inline insert and fallback
index-entry writers already interpret as "all entries changed." This preserves
the append-tail fallback behavior for tables without catalog index roots.

## Affected Subsystems

- First-party MyLite storage append-row path.
- Active statement table-entry and table-index-root absence caches.
- Storage-smoke prepared insert performance baseline.

## Compatibility Impact

No SQL-visible behavior changes. Table existence is still validated by
`find_table_id_in_statement()`, and maintained-root planning still runs when
catalog roots may exist.

## DDL Metadata Routing Impact

No DDL metadata routing behavior changes.

## Single-File And Embedded Lifecycle Impact

No file-lifecycle change. The cache is process-local active checkpoint state.
Durable bytes remain in the primary `.mylite` file plus existing journal
companions.

## Public API, File Format, And Routing Impact

No public C API, durable file-format, wire-protocol, or storage-engine routing
impact.

## Build, Size, And Dependencies

No dependency or license change. Binary impact is limited to first-party C
storage code.

## Test Plan

- Reuse the active append-buffer indexed insert savepoint test to cover cached
  table resolution inside a transaction.
- Build and run `mylite_storage_test`.
- Build and run the storage-smoke embedded storage-engine test.
- Run focused storage-smoke `ctest` coverage for storage and embedded storage.
- Run `prepared-insert-components` before and after the change.
- Run `git diff --check`.
- Run `git clang-format --diff` on changed C files.

## Acceptance Criteria

- Inserts into tables without catalog index roots avoid repeated catalog reads
  after the active absence cache is populated.
- Inserts into tables with maintained roots still plan and update those roots.
- Indexed insert visibility, savepoint rollback, transaction commit, and reopen
  behavior remain covered.
- The local prepared insert `step` metric improves or stays within noise.
- Existing focused storage and embedded storage tests pass.

## Verification Results

Local environment: macOS worktree, `storage-smoke-dev` preset.

- Before this slice, after the append-buffer reservation slice,
  `prepared-insert-step` samples were `4.871`, `4.870`, and `4.844 us/op`.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`:
  passed.
- `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|embedded-storage-engine' --output-on-failure`: passed.
- Three post-change runs of
  `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-insert-components 1000 10000` reported
  `prepared-insert-step` at `4.411`, `4.185`, and `3.738 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-updates
  1000 10000` reported prepared inserts at `6.853 us/op` and prepared
  primary-key updates at `2.012 us/op`.
- `git diff --check`: passed.

## Risks And Open Questions

- This only helps active transactions that repeatedly insert into the same
  table. Autocommit inserts still need catalog resolution per statement.
- Catalog mutations must continue clearing active table and root caches; this
  slice relies on existing cache-retarget/clear paths rather than adding new
  invalidation surfaces.
