# Index Cursor Materialization Flag

## Problem

Prepared primary-key updates repeatedly build an exact unique index cursor
before updating the row. `ha_mylite::build_index_cursor()` already computes
whether row payloads can be materialized inline with
`materialize_index_rows`, but the raw exact unique path walks the table field
list again through `mylite_table_has_blob_fields(table)` before deciding
whether to fetch the durable row payload in the same storage lookup.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to the
  MyLite storage handler in `mariadb/storage/mylite/ha_mylite.cc`.
- `ha_mylite::build_index_cursor()` sets `materialize_index_rows` to true only
  when the table has no BLOB/TEXT fields.
- The exact unique durable fast path can inline the row payload only under the
  same no-BLOB condition, plus non-volatile storage.
- `mylite_table_has_blob_fields()` walks `TABLE::field`, so recomputing the
  same condition adds per-cursor work on prepared point updates.

## Design

- Reuse `materialize_index_rows` when computing `inline_durable_row`.
- Keep the existing volatile-row and BLOB/TEXT safeguards unchanged.
- Do not alter cursor shape, row materialization, storage lookup, or fallback
  behavior.

## Compatibility Impact

No SQL, C API, handler capability, storage-engine routing, file-format, or
durability behavior changes. This is a handler-local fast path cleanup.

## Single-File And Lifecycle Impact

No file lifecycle changes. The same storage APIs and read-statement scope are
used.

## Tests And Verification

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

- Exact unique durable cursor construction does not rescan the table fields
  after `materialize_index_rows` has already captured the no-BLOB condition.
- Existing storage-smoke coverage passes.
- Prepared-update timing is recorded, and any apparent regression is
  investigated before claiming a speedup.
