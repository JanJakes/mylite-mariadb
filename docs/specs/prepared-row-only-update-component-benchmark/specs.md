# Prepared Row-Only Update Component Benchmark

## Problem

The prepared update component benchmark measures
`UPDATE perf_rows SET value = value + 1 WHERE id = ?`. That is the main
indexed-update hot path because `value` has a secondary index, but it combines
MariaDB prepared-update execution cost with MyLite secondary-index replacement
work.

The storage benchmark already separates row-only and indexed mutation
components. The SQL-layer prepared benchmark needs the same split before deeper
prepared-DML reuse work can claim whether a gain came from MariaDB execution
reuse or from avoiding secondary-index maintenance.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `tools/mylite_perf_baseline.c::benchmark_prepared_update_components()` uses
  `SET value = value + 1 WHERE id = ?`, and `value` is indexed in the
  `perf_rows` schema.
- `tools/mylite_perf_baseline.c::benchmark_prepared_assignment_update_components()`
  uses `SET value = ? WHERE id = ?`, which isolates simple assignment setup
  but still exercises secondary-index replacement.
- MyLite storage already has separate `storage-row-update-components` and
  `storage-indexed-row-update-components` phases.

## Design

- Add a focused `prepared-row-only-update-components` phase.
- Create a separate `perf_row_only_rows` table with a primary key, a
  non-indexed integer `counter`, and no secondary indexes.
- Use `UPDATE perf_row_only_rows SET counter = counter + 1 WHERE id = ?`.
- Split bind, step, and reset timings with separate threshold metric names.
- Keep existing indexed prepared update phases unchanged.

## Compatibility Impact

No product behavior change. This is measurement-only.

## Single-File And Lifecycle Impact

No storage format, sidecar, lock, journal, or lifecycle change.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Storage-Engine Routing Impact

The phase still uses `ENGINE=InnoDB` routed through the MyLite storage engine.
The measured update targets a non-indexed integer column so the direct-update
route can exercise row-only storage mutation under the same prepared exact-key
SQL path and value-expression setup shape as the indexed expression phase.

## Test Plan

- Build `mylite_perf_baseline`.
- Run `mylite_perf_baseline --phase=prepared-row-only-update-components`.
- Run the existing indexed assignment and expression phases for comparison.
- Run the storage-smoke test preset.

## Verification

- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline`
  passed.
- `mylite_perf_baseline --phase=prepared-row-only-update-components 1000
  10000` passed and reported bind, step, reset, checksum, and ordered-scan
  output.
- Isolated `prepared-row-only-update-components 10000 1000000` measured the
  step component at 1.725 us/op.
- Isolated `prepared-update-components 10000 1000000` measured the indexed
  expression step component at 1.826 us/op in the same local run window.
- A delayed sample of the row-only phase confirmed the direct-update path was
  used and routed storage mutation through
  `mylite_storage_update_row_preserving_index_entries_in_statement()`, while
  `open_tables_for_query()`, `Sql_cmd_update::prepare_inner()`,
  `JOIN::prepare()`, value-list `setup_fields()`, and table locking remained
  visible.

## Acceptance Criteria

- The new phase reports bind, step, reset, row-only checksum, and ordered-scan
  rows.
- Existing phase names and threshold metric names continue to work.
- Row-only, assignment-indexed, and expression-indexed update phases remain
  separate in docs and output.
