# Handler Table Hot Property Cache

## Problem

Prepared primary-key update profiling still shows per-row time in
`ha_mylite::update_row()` for table capability checks and auto-increment field
discovery. For ordinary non-auto-increment tables, each update currently
rechecks that the table supports MyLite row writes, scans for an
auto-increment field through `mylite_auto_increment_field()`, and then repeats
that absence check before deciding no auto-increment metadata write is needed.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/mylite/ha_mylite.cc` owns the MyLite handler row-DML hot
  path.
- `ha_mylite::update_row()` calls `mylite_table_supports_row_lifecycle()` on
  every update. That helper delegates to `mylite_table_supports_row_write()`,
  which checks supported auto-increment/index shape from immutable table
  metadata.
- `ha_mylite::update_row()` also calls
  `mylite_advance_auto_increment_from_row()` for durable rows and then calls
  `mylite_auto_increment_field()` again before preserving rollback state. For
  tables without auto-increment, both checks rediscover that no such field
  exists.
- Handler `open()` already derives immutable table-owned facts such as the
  storage schema/table name and BLOB presence. The table definition does not
  change for the lifetime of a handler instance.

## Design

- Cache the handler's auto-increment `Field *` at `open()` time and clear it on
  `close()`.
- Cache whether the open table supports MyLite row writes and row lifecycle at
  `open()` time. Hot row-DML paths can then check a boolean instead of walking
  table fields and key metadata.
- Add an auto-increment advance helper that accepts the already-resolved field.
  Keep the existing row-based helper for callers that do not own a handler
  cache.
- Use the cached auto-increment field in durable and volatile row-DML paths
  where the handler owns the table lifetime.

## Scope

In scope:

- Handler-owned row-DML capability checks.
- Handler-owned auto-increment field lookup for insert/update paths.
- Prepared indexed-update performance evidence.

Out of scope:

- Changing auto-increment SQL semantics.
- Changing table metadata publication or DDL support checks.
- Reusing cached properties across handler instances.

## Compatibility Impact

No SQL, public C API, file-format, or storage-engine routing behavior changes.
The cache stores immutable handler table metadata and uses the same existing
support predicates.

## Single-File And Lifecycle Impact

No durable lifecycle change. Auto-increment storage writes still use the
existing statement and transaction rollback paths.

## Test Plan

- Build `mysqlserver`, `mylite_storage_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage-smoke CTest coverage.
- Run `git diff --check` and `git clang-format --diff`.
- Run `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`.

## Acceptance Criteria

- Handler row-DML continues to reject unsupported table shapes.
- Existing auto-increment insert/update and rollback tests pass.
- Prepared indexed updates no longer sample repeated handler table support or
  no-auto-increment field discovery as visible hot work, or the benchmark
  exposes the next hotter bottleneck without a regression.
