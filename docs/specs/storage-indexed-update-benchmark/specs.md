# Storage Indexed Update Benchmark

## Problem

The prepared primary-key update hot path changes the secondary `value_key`
entry while keeping the primary-key entry stable. The existing direct storage
component benchmark measures row-only updates through
`mylite_storage_update_row_preserving_index_entries()`, so it cannot separate
storage mutation cost for the changed-index shape that prepared updates use.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` calls
  `mylite_prepare_checked_index_entries_with_scratch()` and
  `mylite_prepare_index_entry_changes()` when the row update may alter an index
  key image.
- That handler path calls
  `mylite_storage_update_row_with_index_entry_changes()` when at least one
  durable index key changes, while stable-key updates call
  `mylite_storage_update_row_preserving_index_entries()`.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries()`
  dispatches changed-index updates through maintained-root planning, active
  buffered rewrite, inline append, exact-index cache maintenance, row-payload
  cache maintenance, and deferred durable cache retargeting.
- `tools/mylite_perf_baseline.c::benchmark_prepared_update_components()` keeps
  the SQL shape as `UPDATE perf_rows SET value = value + 1 WHERE id = ?`, so
  the direct storage benchmark should use the same table and a stable primary
  key plus changed secondary key.

## Design

- Add an opt-in `storage-indexed-row-update-components` benchmark phase.
- Reuse the existing `perf_rows` table with primary key `id` and secondary key
  `value_key`.
- Load primary-key entries, secondary-key entries, and row payloads once before
  the timed loop.
- Run one outer storage transaction and one nested storage statement per
  iteration, matching the prepared update storage checkpoint shape.
- Call `mylite_storage_update_row_with_index_entry_changes()` with two index
  entries:
  - primary key unchanged,
  - secondary key changed.
- Toggle each row between two existing secondary key byte strings. This keeps
  every timed mutation on the changed-index storage path without requiring the
  benchmark to duplicate MariaDB record or key serialization logic.
- Report nested begin, mutation, and nested commit timings separately.

## Compatibility Impact

No SQL, C API, storage-engine routing, or file-format behavior changes. This is
local performance tooling.

## Single-File And Embedded Lifecycle Impact

No lifecycle change. The benchmark exercises existing nested checkpoint and
transaction behavior against one `.mylite` file.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. The benchmark uses the same routed `ENGINE=InnoDB` table as
the prepared update phase, then calls the MyLite storage API directly for the
measured update loop.

## Binary-Size And Dependency Impact

The benchmark binary gains one diagnostic phase. No library binary-size or
dependency impact.

## Tests And Verification

- Build `mylite_perf_baseline`.
- Run `storage-indexed-row-update-components`.
- Run `storage-row-update-components` and `prepared-update-components` for
  comparison.
- Run storage-smoke tests, `git diff --check`, and clang-format diff checks.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff -- tools/mylite_perf_baseline.c`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-indexed-row-update-components 1000 100000`
  reported nested statement begin at `0.024 us/op`, mutation at
  `0.313 us/op`, and nested statement commit at `0.055 us/op`, followed by a
  successful ordered scan.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-row-update-components 1000 100000`
  reported row-only nested statement begin at `0.027 us/op`, mutation at
  `0.305 us/op`, and nested statement commit at `0.054 us/op`, followed by a
  successful ordered scan.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 100000`
  reported prepared update bind at `0.023 us/op`, step at `1.948 us/op`, and
  reset at `0.023 us/op`, followed by a successful ordered scan.

## Acceptance Criteria

- `tools/mylite-perf-baseline --phase=storage-indexed-row-update-components`
  is accepted by argument parsing.
- The phase reports begin, mutation, and commit timings separately.
- The existing row-only storage update and prepared update component phases
  remain available.
- Local verification records changed-index storage mutation timing beside the
  row-only and routed prepared update component timings.

## Risks And Unresolved Questions

- The phase deliberately measures storage mechanics, not SQL expression
  evaluation. It toggles secondary key bytes from existing key entries instead
  of reserializing MariaDB records.
- If the changed-index storage mutation component is already small relative to
  prepared execution, the next optimization should stay in the handler or SQL
  prepared-update path.
