# Handler Supported Index Entry Precheck

## Problem

After handler table-support caching, prepared indexed updates still spend
visible time inside `mylite_prepare_index_entries_with_scratch()`. The helper
rechecks `mylite_table_supports_indexes()` for every row mutation even though
`ha_mylite::write_row()` and `ha_mylite::update_row()` already reject
unsupported table shapes using the handler-open cache.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `ha_mylite::open()` now caches whether a table supports MyLite row writes.
- `ha_mylite::write_row()` checks that cache before serializing index entries.
- `ha_mylite::update_row()` checks the row-lifecycle cache before serializing
  replacement index entries.
- `mylite_prepare_index_entries_with_scratch()` remains a shared helper used
  by both hot checked handler paths and colder standalone paths.

## Design

- Add a checked helper variant that accepts an `indexes_known_supported` flag.
- Keep the existing public-in-file helper behavior for callers that have not
  already checked table support.
- Route handler `write_row()` and `update_row()` through the checked variant
  after their cached row-write or row-lifecycle guard succeeds.

## Scope

In scope:

- Handler-owned durable and volatile insert/update index-entry serialization.
- Prepared indexed-update performance evidence.

Out of scope:

- Changing supported index definitions.
- Changing duplicate-key, FK, or storage update semantics.

## Compatibility Impact

No SQL, public C API, storage-engine routing, or file-format behavior changes.
Unsupported index shapes still reject before row-DML serialization through the
same predicate, just once per handler open instead of once per row.

## Single-File And Lifecycle Impact

No durable lifecycle change.

## Test Plan

- Build `mysqlserver`, `mylite_storage_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage-smoke CTest coverage.
- Run `git diff --check` and `git clang-format --diff`.
- Run `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`.

## Acceptance Criteria

- Existing unsupported index tests still pass.
- Handler insert/update paths skip repeated index-support scans after their
  cached table support guard.
- Prepared indexed-update benchmark does not regress and profiling moves the
  visible bottleneck to unavoidable key serialization or higher MariaDB
  executor overhead.
