# Direct Update Compact Old Row Snapshot

## Problem

The accepted MyLite exact-key direct-update path still copies the full old
record into `record[1]` before applying update values to `record[0]`.
A current `prepared-row-only-update-components` sample over 10000 rows and
20000000 iterations measured the step component around `1.57 us/op` and still
showed visible time in `store_record(table, record[1])`, `compare_record()`,
and the nested `ha_mylite::update_row()` path after the exact row is already
materialized.

For the common stable-index row-only update shape, the full old row is needed
only for unchanged-row detection and for the generic `update_row()` API. Once
the accepted direct-update statement has proven that no index entry, foreign-key
key, hidden index, or auto-increment state can change, storage can write the new
payload through the existing preserving-index row update path without retaining
the whole old row image.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc::compare_record()` preserves unchanged-row
  affected-count semantics by comparing `record[0]` against `record[1]`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows()` reads
  the exact row into `record[0]`, calls `store_record(table, record[1])`, fills
  update values into `record[0]`, checks `compare_record()`, and then calls
  `ha_mylite::update_row(record[1], record[0])`.
- `ha_mylite::direct_update_rows_init()` already computes
  `direct_update_may_change_index_entries` for the accepted update field set.
- `ha_mylite::update_row()` skips FK checks when
  `direct_update_row_in_progress && !direct_update_may_change_index_entries`
  and uses `mylite_storage_update_row_preserving_index_entries*()` for that
  shape.
- `handler::ha_direct_update_rows()` is already the MariaDB handler wrapper for
  direct updates, so a MyLite-local inner write helper does not need to duplicate
  the generic `ha_update_row()` wrapper.

## Design

- Keep the existing full-row path as the fallback for any unsupported or
  uncertain shape.
- For accepted direct updates, use a compact old-value snapshot only when:
  - `direct_update_can_compare_record` is true,
  - storage can preserve all index entries,
  - the table is durable, has no hidden indexes, and has no auto-increment field
    requiring advancement,
  - the table does not require private MariaDB in-server update constraints,
  - every explicitly updated field is a non-null fixed integer field stored in
    the current record buffer.
- Capture the old bytes of those explicitly updated fields before
  `fill_record()`, then compare the captured bytes with the updated field bytes
  afterward. This preserves unchanged-row affected-count behavior for the
  narrow row-only shape without copying the entire record.
- Add a MyLite handler helper that writes the updated row through
  `mylite_storage_update_row_preserving_index_entries*()` using the current
  row id and current active statement scope. The helper is used only for the
  same durable, stable-index shape; otherwise direct updates continue through
  `ha_mylite::update_row()`.

## Affected Subsystems

- MyLite MariaDB storage handler direct-update execution.
- MyLite storage preserving-index row update calls.
- Prepared row-only update performance evidence.

## Compatibility Impact

No SQL-visible behavior change is intended. MariaDB still performs exact-row
lookup, residual condition evaluation, assignment evaluation, strict conversion,
CHECK constraint verification, diagnostics, and affected-row reporting. The
optimization is limited to a stable-index direct-update shape where the generic
old row is not needed for FK, duplicate-key, hidden-index, or auto-increment
work.

Unsupported field types, nullable updates, key-changing updates, volatile rows,
auto-increment tables, hidden-index tables, and private in-server constraint
shapes stay on the existing full-row fallback.

## Single-File And Embedded Lifecycle Impact

No file-format, journal, lock, recovery, or sidecar lifecycle change. The helper
uses the existing MyLite storage row-update API and active statement ownership.

## Public API And File-Format Impact

No public `libmylite` API change and no `.mylite` file-format change.

## Binary-Size And Dependency Impact

No new dependency. The expected binary-size impact is a small handler-only fork
delta. The rebuilt storage-smoke embedded archive measured `21271624` bytes.

## Test And Verification Plan

- Passed `git diff --check`.
- Passed `git clang-format --diff HEAD -- mariadb/storage/mylite/ha_mylite.cc
  mariadb/storage/mylite/ha_mylite.h
  packages/libmylite/tests/embedded_storage_engine_test.c`.
- Passed `cmake --build build/mariadb-mylite-storage-smoke --target
  libmariadbd.a`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_embedded_storage_engine_test mylite_embedded_statement_test
  mylite_perf_baseline`.
- Extended prepared primary-key update coverage with a multi-field non-key
  update that exercises changed and unchanged affected-row behavior.
- Passed focused embedded statement and storage-engine tests with:
  `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-storage-engine|libmylite.embedded-statement'
  --output-on-failure`.
- Passed full `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`:
  - bind: `0.022 us/op`
  - step: `1.574 us/op`
  - reset: `0.022 us/op`
- Ran sampled `prepared-row-only-update-components` with
  `--profile-iterations=20000000`:
  - bind: `0.022 us/op`
  - step: `1.556 us/op`
  - reset: `0.022 us/op`
  - sample file:
    `/tmp/mylite-direct-update-compact-old-row-snapshot-1779556602.sample.txt`
  - the accepted row-only direct-update branch now uses
    `ha_mylite::direct_update_row_preserving_index_entries()` and no longer
    shows `store_record(table, record[1])`, `compare_record()`, or
    `ha_mylite::update_row()` in that branch.

## Acceptance Criteria

- Eligible direct row-only updates skip full old-record copying and still
  preserve changed/no-op affected-row behavior.
- Unsupported shapes continue through the existing full-row fallback.
- Existing storage-smoke tests pass.
- Local performance evidence shows no material regression and the hot sample
  moves work out of the full-row copy path.

## Risks

- A too-broad compact snapshot could miss unchanged-row behavior for field
  types with out-of-record bytes or custom comparison rules. The first slice
  therefore accepts only non-null fixed integer fields.
- Bypassing `ha_mylite::update_row()` is safe only for the stable-index durable
  direct-update shape. The helper must reject any row shape that needs generic
  old/new row handling.
