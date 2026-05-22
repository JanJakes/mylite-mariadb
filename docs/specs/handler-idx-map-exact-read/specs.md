# Handler Index Read Idx Map Exact Read

## Problem

Prepared primary-key point selects still spend most of their local runtime in
MariaDB's per-execution select optimization. The sampled hot path for:

```sql
SELECT value FROM perf_rows WHERE id = ?
```

uses MariaDB's const-table optimization, which calls
`handler::ha_index_read_idx_map()` and then
`ha_mylite::index_read_idx_map()` during `make_join_statistics()`. MyLite's
ordinary `index_read_map()` already has a guarded direct exact-unique row
materialization path, but `index_read_idx_map()` falls through to
`build_index_cursor()`. That rebuilds cursor storage and pays the storage
read-statement begin/end cost even when a full non-null unique raw key can be
materialized directly into MariaDB's record buffer.

The local baseline before this slice showed:

- `tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 10000`:
  `7.550 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 1000 1000000`: `7.423 us/op`.
- The corresponding sample showed `ha_mylite::index_read_idx_map()` calling
  `ha_mylite::build_index_cursor()`, with the storage read-statement lifecycle
  below it.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_select.cc::make_join_statistics()` reads const tables by
  calling handler index-read APIs during optimization.
- The prepared point-select sample enters
  `handler::ha_index_read_idx_map()`, then
  `ha_mylite::index_read_idx_map()`, then
  `ha_mylite::build_index_cursor()`.
- `mariadb/storage/mylite/ha_mylite.cc::index_read_map()` already calls
  `read_exact_unique_index_row_into()` for exact full-key lookups before it
  builds a cursor.
- `mariadb/storage/mylite/ha_mylite.cc::read_exact_unique_index_row_into()`
  only applies to durable, non-BLOB, supported, non-null unique full-key raw
  filters and otherwise reports `out_applied=false`.
- `mariadb/storage/mylite/ha_mylite.cc::index_read_idx_map()` currently lacks
  the same guarded direct path.

## Design

Apply `read_exact_unique_index_row_into()` from `index_read_idx_map()` under the
same conditions already used by `index_read_map()`:

- only for `HA_READ_KEY_EXACT`;
- only when `calculate_key_len()` produces a nonzero full key;
- only when the helper reports that it applied.

If the helper finds the row, return success with the row already copied into
`buf`. If the helper applies and reports no row, return `HA_ERR_KEY_NOT_FOUND`.
All unsupported, prefix, range, nullable, BLOB, volatile, and non-raw key
shapes keep the existing `build_index_cursor()` path.

## Affected Subsystems

- MyLite MariaDB handler index read path.
- Prepared and direct SQL point selects that MariaDB routes through
  `ha_index_read_idx_map()`.
- Storage-smoke performance baseline.

## Compatibility Impact

The accepted fast path is the same row candidate set as the existing
`index_read_map()` direct path: at most one row for a full non-null unique key.
MariaDB still owns const-table planning, expression semantics, selected column
projection, diagnostics, and no-row behavior.

Unsupported key shapes keep the old cursor-building behavior.

## Single-File And Embedded Lifecycle Impact

No durable file-format or companion-file change. The direct path still opens a
scoped MyLite read statement for the exact durable lookup and preserves the
same journal recovery, shared-lock, and cached checkpoint rules.

## Public API And File-Format Impact

No public API or file-format change.

## Storage-Engine Routing Impact

All durable routed engines can benefit when MariaDB calls
`ha_index_read_idx_map()` for a supported full unique key, including requested
`ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, omitted/default engines, and
explicit `ENGINE=MYLITE`.

## Build, Size, And Dependencies

No new dependency. Binary impact is limited to one extra guarded call in the
MyLite handler.

## Test Plan

- Rebuild the storage-smoke MariaDB embedded archive because
  `mariadb/storage/mylite/ha_mylite.cc` changes.
- Build the storage-smoke handler and performance targets.
- Run storage-smoke tests that cover routed prepared primary-key selects and
  storage-engine behavior.
- Run the local prepared primary-key select benchmark.
- Run `git diff --check` and C++ formatting checks.

## Acceptance Criteria

- Existing routed storage tests pass.
- Prepared primary-key point selects preserve row values and no-row behavior.
- The local prepared primary-key select benchmark improves or stays neutral.
- Unsupported key shapes still go through the existing cursor path.

## Verification Results

- `BUILD_DIR=build/mariadb-mylite-storage-smoke
  tools/mariadb-embedded-build build libmariadbd.a`: passed. Recompiling the
  touched handler emitted existing upstream missing-`override` warnings from
  MariaDB headers.
- `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --preset storage-smoke-dev --output-on-failure -R
  'libmylite.embedded-storage-engine|libmylite.embedded-statement'`: passed.
- `ctest --preset storage-smoke-dev --output-on-failure`: passed, 10 tests.
- `tools/mylite-perf-baseline --phase=prepared-pk-selects 1000 10000`:
  prepared primary-key point selects were `7.736 us/op`, within local noise of
  the recorded `7.550 us/op` short baseline.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-pk-selects 1000 1000000`: prepared primary-key point
  selects were `7.553 us/op`, within local noise of the recorded
  `7.423 us/op` long baseline.
- The post-change sample showed `ha_mylite::index_read_idx_map()` calling
  `ha_mylite::read_exact_unique_index_row_into()` instead of
  `ha_mylite::build_index_cursor()` for the hot const-table read.
- `git diff --check`: passed.
- `git-clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc`: no
  formatting changes.

## Risks And Follow-Up

This does not remove MariaDB's const-table optimization work. It only makes the
handler read called by that optimization cheaper for a shape MyLite already
knows how to materialize directly. Broader prepared SELECT performance still
needs plan-cache or statement-runtime work if optimizer overhead remains the
dominant cost.
