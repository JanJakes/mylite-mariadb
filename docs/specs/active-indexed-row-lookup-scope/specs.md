# Active Indexed Row Lookup Scope

## Problem

Prepared exact-key updates already execute inside an active MyLite statement
checkpoint, but the handler still reads the target row through the generic
filename-based indexed-row API. A delayed local sample of
`prepared-update-components 10000 1000000` after the direct-update key-support
cache still showed `open_existing_file_scope()`, `close_existing_file_scope()`,
and `find_indexed_row_payload()` below `mylite_storage_find_indexed_row_into()`.

This slice removes the generic file-scope open/close wrapper from active
statement row lookups. It does not change exact-index lookup algorithms,
payload decoding, update-page rewrite, or checkpoint ownership.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::read_exact_unique_index_row_into()`
  performs accepted exact unique-key handler reads for both ordinary index
  reads and MyLite direct-update target-row reads.
- `packages/mylite-storage/src/storage.c::mylite_storage_find_indexed_row_into()`
  validates public filename/schema/table/key inputs and then calls
  `find_indexed_row_payload()`.
- `find_indexed_row_payload()` opens a `mylite_storage_file_scope`, derives the
  active cache owner from that scope, reads the current header/table entry,
  resolves an exact index row id, reads the payload, and closes the scope.
- For direct-update row-DML, `libmylite` or the handler has already opened an
  active statement checkpoint. `open_existing_file_scope()` therefore borrows
  the active statement file and `close_existing_file_scope()` only clears
  stream state; both are avoidable when the caller already has the statement.
- The storage header exposes the opaque `mylite_storage_statement` type and
  statement lifecycle APIs, but not a borrowed active-statement lookup helper or
  an indexed-row read API scoped to a borrowed active statement.

## Design

- Add a borrowed active-statement helper:
  `mylite_storage_borrow_active_statement(const char *filename)`.
  - It returns the current owner-matching active write statement for the
    filename, or `NULL` when there is no matching active statement.
  - The caller must not commit, rollback, end, or store the borrowed pointer.
- Add an active-statement indexed-row read API:
  `mylite_storage_find_indexed_row_in_statement_into()`.
  - It accepts a borrowed `mylite_storage_statement *`, schema/table names,
    index number, exact key bytes, and caller-owned output row storage.
  - It uses the active statement's file, current header, table-entry cache,
    exact-index cache, live-row validation cache, and row-payload cache.
  - It returns the same storage result values as
    `mylite_storage_find_indexed_row_into()`.
- Refactor the existing filename-based public lookup to share the same internal
  helper, keeping raw public callers on the existing file open/recovery/lock and
  close path.
- In `ha_mylite::read_exact_unique_index_row_into()`, use the active-statement
  scoped API when a current storage-context statement or handler-owned THD
  checkpoint is available; otherwise keep the current read-scope and
  filename-based API path.

## Affected Subsystems

- MyLite storage public C API.
- MyLite storage exact-index row lookup internals.
- MyLite MariaDB storage-engine handler direct/index row-read path.

## Compatibility Impact

No SQL result, warning, error, or affected-row behavior should change. The
active path uses the same table-entry, exact-index, payload, and live-row
validation helpers as the filename-based path.

## DDL Metadata Routing Impact

No DDL metadata routing change. The active path reuses the active checkpoint's
current catalog image and table-entry cache.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle change.
The active path is valid only while the caller's existing statement checkpoint
already owns the single `.mylite` file handle.

## Public API And File-Format Impact

The storage C API gains one borrowed active-statement helper and one scoped
indexed-row read helper. The `.mylite` file format is unchanged.

## Storage-Engine Routing Impact

No engine-routing policy change. The handler still reaches this path only after
MyLite has accepted a routed exact unique-key row lookup.

## Wire-Protocol Or Integration-Package Impact

None.

## Binary-Size Impact

Expected to be neutral to very small positive/negative noise: the existing row
lookup body is shared, and only two small exported helpers are added.

## License Or Dependency Impact

None.

## Tests And Verification

- `git diff --check`
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc
  packages/mylite-storage/include/mylite/storage.h
  packages/mylite-storage/src/storage.c`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `2/2` tests passed.
- `ctest --preset storage-smoke-dev --output-on-failure`
  - `10/10` tests passed.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build
  all -DPLUGIN_MYLITE_SE=STATIC`
  - `size_bytes=21186976`
  - `size_mib=20.21`
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-update-components 10000 1000000`
  - prepared primary-key update bind component: `0.023 us/op`
  - prepared primary-key update step component: `2.124 us/op`
  - prepared primary-key update reset component: `0.023 us/op`
- Delayed local sample of the same phase showed
  `mylite_storage_find_indexed_row_in_statement_into()` on the accepted target
  row read and no `mylite_storage_find_indexed_row_into()`,
  `open_existing_file_scope()`, or `close_existing_file_scope()` under that
  path. Remaining sampled frames included `rewrite_active_update_pages`,
  `find_exact_index_row_id`, `read_indexed_row_payload_from_open_file`,
  `find_indexed_row_payload_in_scope`, `fill_record`, and
  `ha_mylite::direct_update_rows()`.

## Acceptance Criteria

- Accepted direct-update target-row reads use the active-statement scoped
  indexed-row API when a matching active statement exists.
- Raw filename-based indexed-row callers keep existing recovery, lock, and
  close semantics.
- Prepared exact-key update behavior remains covered by existing embedded
  storage-engine tests.
- Delayed prepared-update samples no longer show `open_existing_file_scope()` or
  `close_existing_file_scope()` under the accepted active target-row read.

## Risks And Unresolved Questions

- Borrowed active statement pointers must not outlive the active checkpoint.
  The handler will use the pointer only for the immediate row lookup.
- The active scoped API must not bypass cross-process recovery for raw callers;
  raw callers keep the existing filename-based path.
