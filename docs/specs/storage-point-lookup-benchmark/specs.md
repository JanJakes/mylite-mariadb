# Storage Point Lookup Benchmark

## Problem

The local benchmark reports routed prepared primary-key point selects through
the full MariaDB prepared-statement path. That is the user-visible path, but it
does not separate storage lookup cost from MariaDB executor, handler, prepared
result, and reset overhead.

Recent storage metadata and allocation experiments did not improve the routed
prepared point-select metric. Before continuing optimization work, the project
needs a storage-level point-lookup benchmark that uses the same `.mylite`
database and MyLite storage API directly.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `tools/mylite_perf_baseline.c` already creates a routed
  `ENGINE=InnoDB` table, inserts deterministic rows, and measures direct and
  prepared SQL point selects.
- `packages/mylite-storage/include/mylite/storage.h` exposes
  `mylite_storage_read_index_entries()` and
  `mylite_storage_find_indexed_row_into()`.
- `mariadb/storage/mylite/ha_mylite.cc::read_exact_unique_index_row_into()`
  calls `mylite_storage_find_indexed_row_into()` after constructing the
  MariaDB key buffer.
- `mylite_storage_find_indexed_row_into()` accepts serialized storage key bytes,
  so a benchmark can source those bytes from the stored primary index entries
  rather than duplicating MariaDB key serialization in the tool.

## Design

- Add `storage-pk-entry-lookups`, `storage-pk-entry-lookups-one-read`,
  `storage-pk-row-lookups`, and `storage-pk-row-lookups-one-read` phases to
  `tools/mylite_perf_baseline`.
- After normal setup and row insertion, read primary-key index entries for
  `perf.perf_rows` index `0` before the timed section.
- Use the stored key bytes as lookup inputs and call
  `mylite_storage_find_index_entry()` or
  `mylite_storage_find_indexed_row_into()` directly for each iteration.
- Wrap each lookup in `mylite_storage_begin_read_statement()` /
  `mylite_storage_end_read_statement()` so the benchmark includes the same
  storage read-scope setup used by the handler exact read path.
- For `*-one-read` phases, open one storage read statement around the timed
  loop to isolate steady-state lookup cost when file, header, and catalog state
  are already scoped by one read statement.
- Add `storage-read-statements` to time the begin/end pair directly against the
  same populated benchmark file.
- Verify each lookup returns the expected row id from the stored primary-key
  entryset; the row phase also verifies a row payload size.
- Report row-id checksum and, for the row phase, row-size checksum to keep the
  compiler from optimizing the loop away and to catch obvious lookup drift.

## Compatibility Impact

No SQL-visible behavior changes. This is benchmark-only instrumentation.

## Single-File And Embedded Lifecycle Impact

No durable file, journal, lock, recovery, catalog, or companion-file lifecycle
change.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing-policy change. The benchmark explicitly measures storage underneath
the existing routed table.

## Binary-Size And Dependency Impact

Benchmark-tool-only code. No dependency change.

## Tests And Verification

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build build libmariadbd.a`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --help`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-entry-lookups 10000 1000000`
  - `storage primary-key entry lookups`: `4.152 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-entry-lookups-one-read 10000 1000000`
  - `storage primary-key entry lookups in one read statement`: `0.173 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-row-lookups 10000 1000000`
  - `storage primary-key row lookups`: `4.747 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-pk-row-lookups-one-read 10000 1000000`
  - `storage primary-key row lookups in one read statement`: `0.508 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-read-statements 10000 1000000`
  - `storage read statement begin/end pairs`: `3.910 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-pk-selects 10000 1000000`
  - `prepared primary-key point selects`: `7.845 us/op`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-scalar-selects 10000 1000000`
  - `prepared scalar selects`: `0.738 us/op`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `git diff --check`
- `git clang-format --diff -- tools/mylite_perf_baseline.c`

## Acceptance Criteria

- `tools/mylite_perf_baseline --help` lists the storage phases and metrics.
- The storage phases verify row ids, and the row phases verify row payload
  sizes, for repeated storage point lookups.
- Benchmark output gives a storage-level lower-level comparison point for the
  existing prepared SQL point-select phase.

## Risks And Unresolved Questions

- The benchmark depends on the primary-key index entry order generated by the
  current deterministic insert workload. It verifies row ids from the entryset
  rather than assuming row ids equal SQL primary-key values.
- Reading the index entryset before timing may warm storage leaf-page caches.
  That is acceptable for this phase because repeated prepared point-select
  loops also measure steady-state lookup cost after the first few iterations.
- This is evidence for prioritization, not a product optimization by itself.
