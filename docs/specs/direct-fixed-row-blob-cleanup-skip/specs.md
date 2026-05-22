# Direct Fixed-Row BLOB Cleanup Skip

## Problem

The direct exact unique handler read path only applies to durable tables without
BLOB fields. After `mylite_storage_find_indexed_row_into()` copies the fixed
row directly into MariaDB's record buffer, the handler still resolves a record
BLOB payload slot and clears slot-owned payload state.

For this accepted shape the table has no BLOB fields, so no BLOB payload state
can be associated with either MariaDB record buffer. The slot lookup and clear
are defensive work on a hot primary-key point-read path.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/storage/mylite/ha_mylite.cc::read_exact_unique_index_row_into()`
  returns without applying the direct path when `table_has_blob_fields` is true.
- `record_blob_payload_slot()` exists to select the handler-owned BLOB payload
  storage for `table->record[0]` or `table->record[1]`.
- BLOB-capable paths in `read_index_cursor_row()`, `rnd_next()`, and
  `preserve_record_blob_payloads()` still need the slot cleanup and payload
  ownership logic.
- The direct exact unique path already validates that the copied row payload
  size equals `table->s->reclength`, matching fixed-record tables.

## Design

- Remove `record_blob_payload_slot()` and payload-slot clearing from
  `read_exact_unique_index_row_into()` after successful direct fixed-row reads.
- Add a debug assertion that the handler has no record BLOB payloads in this
  no-BLOB accepted shape.
- Keep all BLOB-capable cursor, scan, random-position, and DML preserve paths
  unchanged.

## Compatibility Impact

No SQL-visible behavior should change. Tables with BLOB fields never take this
direct path, and fixed-record tables do not own separate BLOB payload buffers.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing-policy change. The change applies only after a durable routed table
has already qualified for the direct fixed-row exact unique read path.

## Binary-Size And Dependency Impact

Small handler code removal. No dependency change.

## Tests And Verification

- Rebuild the MariaDB storage-smoke archive because
  `mariadb/storage/mylite/ha_mylite.cc` changes.
- Rebuild storage-smoke embedded storage-engine and performance targets.
- Run storage-smoke embedded storage-engine coverage and the full
  storage-smoke preset.
- Run prepared primary-key point-select and scalar benchmark phases.
- Run formatting and whitespace checks.

## Acceptance Criteria

- Direct fixed-row exact unique reads do not resolve or clear a BLOB payload
  slot.
- BLOB-capable paths are unchanged.
- Existing storage-smoke tests pass.
- Local benchmark evidence records routed point-select behavior.

## Verification Evidence

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_embedded_storage_engine_test mylite_perf_baseline`
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  - 1/1 test passed.
- `ctest --preset storage-smoke-dev --output-on-failure`
  - 10/10 tests passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - Prepared primary-key point selects: `9.296 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-scalar-selects 10000 1000000`
  - Prepared scalar selects: `0.709 us/op`.
- `git diff --check`
- `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc`

## Risks And Unresolved Questions

- The optimization depends on `table_has_blob_fields` being stable for a handler
  open, which is already how the existing handler caches table row-shape
  support.
- This is a small hot-path cleanup, not a replacement for navigable indexes or
  pager work.
