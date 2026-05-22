# Direct Update Shape Cache

## Problem

The accepted MyLite direct-update path still repeats small handler-side shape
checks on every prepared exact-key update execution. The storage mutation itself
is already sub-microsecond locally, while the prepared row-only update step is
about 1.8 us/op and remains dominated by MariaDB table-open, lock, prepare, and
handler setup work.

Recent row-only samples still show `ha_mylite::direct_update_rows_init()` in
the steady loop. That function recomputes table-shape facts that are immutable
for a handler over the same table share and write set:

- whether row buffers can be compared with `compare_record()`,
- whether duplicate-key checks can be skipped,
- whether any index entries may change,
- the per-index key-change mask.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_update.cc::Sql_cmd_update::update_single_table()` pushes
  direct-update proof, update fields, and update values to the handler, then
  calls `direct_update_rows_init()` before `ha_direct_update_rows()`.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::direct_update_rows_init()`
  validates the accepted table shape, rejects in-server constraint shapes,
  checks whether updated fields touch direct-unsafe key parts, optionally checks
  FK presence for key-changing updates, and computes direct-update row/index
  flags.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_update_fields_change_direct_unsafe_key()`
  and `mylite_key_fields_may_change()` walk immutable key and field metadata.
- FK presence is epoch-invalidated separately through
  `mylite_foreign_key_presence_epoch`, so this first cache should not skip FK
  presence checks for key-changing updates.

## Design

- Add a handler-local direct-update shape cache guarded by:
  - current `TABLE_SHARE *`,
  - current write-set bitmap,
  - current key count.
- Cache only the accepted non-key-changing shape. If an update field touches a
  key part, keep the existing uncached path so FK-presence checks and
  key-sensitive acceptance stay live on every execution.
- On a cache hit, copy the cached row-comparison, duplicate-check, index-change,
  and per-index key-change-mask values into the current direct-update state.
- Clear the cache on handler open and close. Let ordinary per-statement direct
  condition state clear independently, so a reused handler can keep immutable
  table/field-shape facts across prepared executions.

This is intentionally not a table-open, MDL, JOIN, or expression setup reuse
change. It is a small handler-side follow-up beneath the existing accepted
direct-update path.

## Affected Subsystems

- MyLite storage handler direct-update initialization.
- Prepared row-DML performance for stable non-key updates.

No SQL parser, optimizer, catalog, storage format, or public C API behavior
changes.

## Compatibility Impact

No SQL-visible behavior change is intended. Direct updates still evaluate
MariaDB conditions and assignments, preserve unchanged-row affected-count
behavior, run CHECK/FK-sensitive gates, and fall back for unsupported shapes.

Key-changing updates are deliberately left uncached in this slice so FK metadata
changes and unique-key-sensitive checks keep the existing behavior.

## Single-File And Embedded Lifecycle Impact

No durable file-format, journal, lock, sidecar, or recovery change. The cache is
process-local handler state and is cleared with handler open/close lifecycle.

## Public API And File-Format Impact

No public `libmylite` API or `.mylite` file-format change.

## Binary-Size And Dependency Impact

Adds a few fields and helper methods to the MyLite handler. No dependency.

## Tests And Verification

- Passed `git diff --check`.
- Passed `git clang-format --diff -- mariadb/storage/mylite/ha_mylite.cc
  mariadb/storage/mylite/ha_mylite.h`.
- Passed `cmake --build build/mariadb-mylite-storage-smoke --target
  libmariadbd.a`.
- Passed `cmake --build --preset storage-smoke-dev --target
  mylite_perf_baseline mylite_embedded_storage_engine_test`.
- Passed `ctest --test-dir build/storage-smoke-dev -R
  'libmylite.embedded-storage-engine' --output-on-failure`.
- Passed `ctest --preset storage-smoke-dev --output-on-failure`.
- Ran `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-row-only-update-components 10000 1000000`:
  - bind: `0.021 us/op`
  - step: `1.646 us/op`
  - reset: `0.021 us/op`
- Ran a focused prepared row-only update sample. `direct_update_rows_init()`
  remained a small frame, while `open_tables_for_query()`,
  `Sql_cmd_update::prepare_inner()`, `JOIN::prepare()`, and lock/unlock stayed
  as the dominant remaining SQL-layer wall.
- Ran `prepared-update-components` and
  `prepared-assignment-update-components` after the full test preset. Local
  step samples were `1.719 us/op` and `1.672 us/op`, respectively.

## Acceptance Criteria

- Repeated prepared row-only exact-key updates reuse handler shape state for
  non-key-changing updates.
- Key-changing direct updates keep the existing uncached initialization path.
- Existing direct/prepared routed storage tests pass.
- Prepared update benchmarks do not regress.

## Risks

- Caching key-changing acceptance could skip FK-presence invalidation, so this
  first slice does not do that.
- Reusing table-shape state across a different table share would be unsafe, so
  cache hits require the same `TABLE_SHARE *` and key count.
