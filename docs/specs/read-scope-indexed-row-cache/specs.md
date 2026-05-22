# Read-Scope Indexed Lookup Cache

## Problem

Prepared primary-key point selects keep a storage read statement open across
reset/re-execute loops. `open_existing_file_scope()` correctly borrows that
active read statement, so repeated reads avoid file open, shared-lock, recovery,
header, and catalog-image setup.

`mylite_storage_find_index_entry()` and `find_indexed_row_payload()` still
derive their cache owner only from `file_scope.active_statement`. When the
borrowed file scope came from `file_scope.active_read_statement`, the
table-entry cache and statement exact-index cache are not used. Row-payload
reads also miss the active row-payload cache. Sampling `prepared-pk-selects`
after durable filename identity work still shows `find_table_record()` and
`catalog_image_view_for_file()` under `find_indexed_row_payload()`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::begin_prepared_read_scope()` opens a
  MyLite storage read statement for eligible prepared result statements and
  retains it while reset/re-execute loops are fully drained.
- `mariadb/storage/mylite/ha_mylite.cc::Mylite_read_statement_scope` avoids a
  nested read statement when the current storage context already has an active
  read statement for the primary file.
- `packages/mylite-storage/src/storage.c::open_existing_file_scope()` returns
  borrowed read scopes in `file_scope.active_read_statement`.
- `packages/mylite-storage/src/storage.c::mylite_storage_find_index_entry()`
  and `find_indexed_row_payload()` compute `active_cache_statement` only from
  `file_scope.active_statement`, so it is `NULL` for read-scope point selects.
- `find_active_table_entry_cache_in_statement()`,
  `find_exact_index_row_id()`, and
  `active_row_payload_cache_for_resolved_statement()` can already use a caller
  supplied statement cache owner.

## Design

Use the active read statement as the cache owner for indexed lookups when there
is no active write statement:

- Keep active write statement behavior unchanged.
- In `mylite_storage_find_index_entry()` and `find_indexed_row_payload()`,
  derive `active_cache_statement` from `file_scope.active_statement` when
  present, otherwise from `file_scope.active_read_statement`.
- Continue passing `file_scope.active_statement` as the mutation statement to
  exact-index lookup so deferred durable-cache retarget checks keep their
  write-checkpoint semantics.
- Leave non-indexed read paths unchanged until profiling justifies broadening
  the same pattern.

## Compatibility Impact

No SQL-visible, handler API, `libmylite` public API, metadata routing, engine
routing, or diagnostics behavior changes. The change only stores and reuses
transient cache entries inside a read statement whose header/catalog view is
already fixed for the read scope.

## Single-File And Lifecycle Impact

No file-format, journal, lock, recovery, or companion-file change. Read-scope
caches are process-local and are discarded with the existing read statement.

## Binary Size And Dependency Impact

No dependency change. Binary-size impact is negligible.

## Test And Verification Plan

- Add a storage test hook that can verify a read statement owns a cached table
  entry after an indexed lookup.
- Add storage coverage that begins read statements, performs
  `mylite_storage_find_index_entry()` and
  `mylite_storage_find_indexed_row_into()`, and verifies the read statement
  owns the table-entry cache while lookup results remain correct.
- Run `git diff --check` and `git clang-format --diff`.
- Build `mylite_storage_test` and `mylite_perf_baseline` with
  `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_perf_baseline`.
- Run `ctest --preset storage-smoke-dev -R mylite-storage.capabilities`.
- Run focused benchmarks:
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 1000 100000`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-select-components 1000 100000`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-entry-lookups-one-read 1000 10000`
  - `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-row-lookups-one-read 1000 10000`

## Acceptance Criteria

- Direct exact-index lookups and indexed-row payload reads under an active read
  statement use that read statement as their cache owner.
- Active write-statement cache ownership and mutation semantics are unchanged.
- Storage capability tests pass.
- Prepared point-select benchmarks remain correct and do not materially regress.

## Risks And Open Questions

- The read statement cache is safe only while the read scope owns the current
  header/catalog view. This slice does not introduce a durable cross-statement
  metadata cache.
- This remains a storage-side improvement. The larger prepared SELECT cost is
  still MariaDB statement execution and optimizer work.
