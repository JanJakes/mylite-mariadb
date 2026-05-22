# Storage Row Update Component Benchmark

## Problem

The direct `storage-row-updates` benchmark shows storage row rewrites under
nested statement checkpoints at roughly two orders of magnitude above the
MariaDB-routed prepared update step sample. That aggregate timing is not enough
to choose the next optimization because it combines nested checkpoint begin,
storage mutation, and nested checkpoint commit.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc::StorageStatementCheckpoint::begin()`
  opens a nested storage statement when an outer transaction checkpoint exists.
- `packages/libmylite/src/database.cc::StorageStatementCheckpoint::commit()`
  commits that nested storage statement after successful row-DML execution.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::external_lock()` opens the
  outer transaction checkpoint for transactional row-DML and avoids a duplicate
  handler statement checkpoint when one is already active.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` calls
  `mylite_storage_update_row_preserving_index_entries()` or
  `mylite_storage_update_row_with_index_entry_changes()` for durable row
  mutation.
- `packages/mylite-storage/src/storage.c::mylite_storage_begin_nested_statement()`,
  `update_row_with_index_entries()`, and `mylite_storage_commit_statement()`
  are the direct storage pieces that the benchmark must separate.

## Design

- Add an opt-in `storage-row-update-components` benchmark phase.
- Reuse the `storage-row-updates` setup: `perf_rows`, primary-key row ids, and
  each row's existing storage payload.
- Run the same one outer storage transaction and one nested storage statement
  per iteration.
- Measure three components separately:
  - nested statement begin,
  - `mylite_storage_update_row_preserving_index_entries()`,
  - nested statement commit.
- Keep the aggregate `storage-row-updates` phase unchanged.
- Run the same row-count and ordered-scan verification after the component
  phase.

## Compatibility Impact

No SQL, C API, storage-engine routing, or file-format behavior changes. This is
local performance tooling.

## Single-File And Embedded Lifecycle Impact

No lifecycle change. The phase exercises existing transaction and nested
statement mechanics against one `.mylite` file.

## Public API And File-Format Impact

No public API or `.mylite` file-format change.

## Storage-Engine Routing Impact

No routing change. The phase only adds measurement next to the existing
MariaDB-routed prepared update component benchmark.

## Binary-Size And Dependency Impact

The benchmark binary gains one diagnostic phase. No library binary-size or
dependency impact.

## Tests And Verification

- Build `mylite_perf_baseline`.
- Run `storage-row-update-components`.
- Run `storage-row-updates` to ensure the aggregate phase still works.
- Run `prepared-update-components` for comparison.
- Run `git diff --check` and clang-format diff checks.

## Verification Evidence

- `git diff --check`
- `git clang-format --diff -- tools/mylite_perf_baseline.c`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-row-update-components 1000 100000`
  reported nested statement begin at `0.024 us/op`, mutation at
  `189.459 us/op`, and nested statement commit at `0.097 us/op`, followed by a
  successful ordered scan.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=storage-row-updates 1000 10000`
  reported aggregate storage row updates at `256.119 us/op`, followed by a
  successful ordered scan.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-update-components 1000 100000`
  reported prepared update bind at `0.023 us/op`, step at `2.445 us/op`, and
  reset at `0.028 us/op`, followed by a successful ordered scan.

## Acceptance Criteria

- `tools/mylite-perf-baseline --phase=storage-row-update-components` is
  accepted by argument parsing.
- The phase reports begin, mutation, and commit timings separately.
- Existing `storage-row-updates` and `prepared-update-components` phases still
  run.
- The spec records local evidence that identifies the dominant direct storage
  update component.

## Risks And Unresolved Questions

- The phase still bypasses MariaDB, so it identifies direct storage component
  cost rather than routed SQL component cost.
- If the dominant cost is mutation, a deeper storage micro-split may be needed
  before changing row-state, cache, or journal code.
