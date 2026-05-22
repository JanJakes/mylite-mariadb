# Storage Row Update Benchmark

## Problem

Prepared primary-key updates now spend nearly all measured time inside the
`mylite_step()` execution component. That component includes MariaDB prepared
execution, handler row-update work, nested MyLite statement checkpoints, and
storage row and index mutation.

Before changing the write path again, the performance harness needs a direct
storage update phase that bypasses MariaDB and measures MyLite row mutation
under an explicit one-transaction, per-statement checkpoint shape.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_prepare.cc::Prepared_statement::execute_loop()` sets
  parameters and executes the prepared statement for each loop iteration.
- `mariadb/sql/sql_update.cc` routes ordinary single-table updates through
  `handler::ha_update_row()`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` prepares row
  payload and index-entry change metadata before calling
  `mylite_storage_update_row_with_index_entry_changes()` for indexed updates.
- The same handler calls `mylite_storage_update_row_preserving_index_entries()`
  when the update does not change any index key image.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries()`
  performs scoped file/header setup, live-row validation, nested statement
  journaling, append-buffer rewrite or append, maintained index updates, header
  publication, and cache updates.

## Design

- Add an opt-in `storage-row-updates` benchmark phase.
- Reuse the existing `perf_rows` schema with a primary key and secondary
  `value_key`.
- Load primary-key row ids and each row's current storage payload once during
  benchmark setup.
- Begin one MyLite storage transaction and, for every iteration, begin a nested
  storage statement, call `mylite_storage_update_row_preserving_index_entries()`
  with that row's own payload bytes, then commit the nested statement.
- Exclude the outer transaction commit from the timed loop, matching the
  existing prepared update component timing.

## Compatibility Impact

No SQL or public C API behavior changes. This is local performance tooling.

## Single-File And Embedded Lifecycle Impact

No file-format or lifecycle change. The benchmark exercises existing
transaction and nested-statement storage behavior.

## Public API And File-Format Impact

No public C API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. The phase provides direct storage evidence beside the
existing MariaDB-routed prepared update path.

## Binary-Size And Dependency Impact

The benchmark binary gains one focused phase. No library binary-size or
dependency impact.

## Tests And Verification

- Build `mylite_perf_baseline`.
- Run the new `storage-row-updates` phase.
- Run the prepared update component phase for comparison.
- Run `git diff --check` and clang-format diff checks.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff -- tools/mylite_perf_baseline.c`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-row-updates 1000 100000`
  reported storage row updates at `188.270 us/op`, followed by a successful
  ordered scan.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 100000`
  reported prepared update bind at `0.022 us/op`, step at `2.286 us/op`, reset
  at `0.027 us/op`, followed by a successful ordered scan.

## Acceptance Criteria

- `tools/mylite-perf-baseline --phase=storage-row-updates` is accepted by
  argument parsing.
- The phase reports direct storage row-only update timing.
- The phase keeps the existing prepared update phases unchanged.
- Local verification records storage row-update timing beside the prepared
  update execution component.

## Risks And Unresolved Questions

- The phase is diagnostic. It updates storage rows through the storage API using
  each row's existing payload bytes while preserving index entries, so it
  isolates storage row-mutation mechanics rather than full SQL expression
  evaluation or secondary-index maintenance.
- The measured direct-storage path is much slower than the MariaDB-routed
  prepared update path on the current sample. Treat it as evidence to split
  direct checkpoint overhead from mutation overhead next, not as a SQL lower
  bound.
