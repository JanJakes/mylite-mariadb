# Active Cache Table Entry Scope

## Problem

Prepared primary-key update profiling still shows `active_cache_statement_for()`
on the hot path after nested checkpoint allocation and catalog-copy work was
reduced. `update_row_with_index_entries()` already opens the active storage
statement scope, but then resolves the outer cache-owning checkpoint again by
filename. Table-entry lookup helpers repeat the same filename-based chain walk
when probing and storing the active table-entry cache.

## Source Findings

- Target base: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The affected code is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`.
- `open_existing_file_for_update_scope()` returns the active write statement
  that owns the already-open file for nested statement checkpoints.
- `active_cache_statement_for()` intentionally returns the outermost matching
  active statement so caches survive nested savepoint commit/rollback.
- `update_row_with_index_entries()` already passes the resolved active cache
  statement to exact-index and row-payload cache maintenance, but it computes
  that statement through a second filename walk.
- `find_table_id()`, `find_active_table_entry_cache()`, and
  `store_active_table_entry_cache()` independently rediscover the same cache
  owner from the filename.

## Design

Add statement-scoped table-entry cache helpers.

- Keep the existing filename-based `find_table_id()` as a wrapper for unchanged
  callers.
- Add a helper that derives the outer cache owner from an already-resolved
  active statement by walking its parent chain, avoiding filename comparisons.
- Add an internal read file scope wrapper so exact-index row lookup can reuse
  the active statement found while opening the file.
- Add `find_table_id_in_statement()` so hot paths can pass the resolved cache
  owner directly.
- Update prepared-update and exact-index lookup paths that already resolve an
  active cache statement to reuse it for table-id lookup, table-entry cache
  lookup, and table-entry cache storage.

This preserves the existing outer-checkpoint cache ownership model while
removing avoidable active-statement chain walks from hot row-DML paths.

## Compatibility Impact

No SQL or C API behavior changes. This only changes how internal cache owners
are passed between storage helpers.

## DDL Metadata Routing Impact

No metadata routing behavior changes. Catalog generation and root-page guards
still determine whether a cached table entry is valid.

## Single-File And Lifecycle Impact

No file-format, journal, locking, or companion-file changes.

## Public API Or File-Format Impact

None.

## Storage-Engine Routing Impact

No routing behavior changes. The affected code is internal storage metadata
lookup used by routed durable row-DML and index probes.

## Wire-Protocol Or Integration-Package Impact

None.

## Binary-Size Impact

Negligible: small wrapper helpers and a narrower hot-path call graph.

## License Or Dependency Impact

None.

## Test And Verification Plan

- Build storage-smoke targets.
- Run focused storage and embedded statement/storage-engine tests.
- Run full storage-smoke CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run the prepared-update performance baseline and sample the hot path.

## Acceptance Criteria

- Prepared update row storage reuses the active cache statement derived from the
  already-open update scope.
- Table-entry cache lookup/storage can operate on a caller-supplied active cache
  statement without changing `find_table_id()` behavior for other callers.
- Existing storage, nested rollback, exact-index lookup, and embedded
  storage-engine tests pass.
- Prepared-update performance is neutral or improved, with fewer
  `active_cache_statement_for()` samples in the hot update path.

## Risks And Unresolved Questions

- The active cache owner is deliberately the outermost active checkpoint for the
  same file and owner. The new helper must preserve that behavior, not switch
  caches to the innermost nested savepoint.
- Some non-hot callers still use filename wrappers. Broadly converting them can
  be done later when profiling shows a reason.
