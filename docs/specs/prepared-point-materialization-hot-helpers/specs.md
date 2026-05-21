# Prepared Point Materialization Hot Helpers

## Problem

After exact-index descriptor lookup stopped showing as a sampled frame, focused
prepared primary-key update profiling still showed MyLite-owned helper calls
inside point-row materialization. The visible storage-side frames are tiny
control helpers around cached row-payload copy and active table-entry identity
refresh rather than storage semantics.

Prepared point updates repeatedly use the same handler table, the same active
statement cache, and small fixed-width row images. The remaining helper calls
sit on the hot path after the exact row id is known and before MariaDB receives
the row image in its handler record buffer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite handler direct unique-index reads call
  `mylite_storage_find_indexed_row_into()` from
  `mariadb/storage/mylite/ha_mylite.cc`.
- `mylite_storage_find_indexed_row_into()` routes through
  `find_indexed_row_payload()` and
  `read_indexed_row_payload_from_open_file()` in
  `packages/mylite-storage/src/storage.c`.
- Cached active row-payload hits call `copy_cached_row_payload()` and then
  `copy_row_payload_to_output_target()` before copying into the handler record
  buffer.
- Active table-entry cache hits validate names through the hot inline
  `table_entry_cache_names_match()` helper, then refresh pointer identity
  through `store_table_entry_cache_name_identity()`.
- The row copy itself is required because MariaDB owns the handler record
  buffer. This slice does not try to alias MariaDB record storage to MyLite
  cache memory.

## Design

- Mark cached row-payload copy helpers as `MYLITE_STORAGE_HOT_INLINE` so the
  prepared point-read path can collapse the active-cache hit into the caller.
- Mark table-entry identity refresh as `MYLITE_STORAGE_HOT_INLINE`, and make it
  return immediately when the stored identity is already current.
- Keep the existing validation, buffer-capacity checks, allocation semantics,
  and not-found/error behavior unchanged.
- Do not change handler cursor semantics, `current_row_id` publication, BLOB
  row paths, or MariaDB record ownership.

## Affected Subsystems

- MyLite storage active row-payload cache materialization.
- MyLite storage active table-entry cache identity refresh.
- Prepared primary-key update performance baseline.

## Compatibility Impact

No SQL, public API, handler API, metadata, storage-engine routing, or
file-format behavior change. The same rows are copied into the same output
buffers with the same error results.

## Single-File And Lifecycle Impact

No durable storage, journal, lock, or companion-file lifecycle change. The slice
changes only transient helper call shape.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party code-layout change. No dependency change. Binary-size impact
is expected to be negligible and should be checked through the existing build
artifact when broader size work resumes.

## Tests And Verification

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`.
- Run full `ctest --preset storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.
- Sample a focused prepared-update benchmark and check whether
  `copy_cached_row_payload`, `copy_row_payload_to_output_target`, and
  `store_table_entry_cache_name_identity` remain visible storage frames.
- Run `git diff --check` and `git clang-format --diff` on the touched C file.

Completed verification:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline` passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test` passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  passed, 2/2 tests.
- `ctest --preset storage-smoke-dev --output-on-failure` passed, 10/10 tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000` recorded prepared primary-key updates
  at 2.273 us/op.
- The focused sampled run recorded 2.379 us/op in
  `/tmp/mylite-prepared-point-materialization-hot-helpers.sample`.
  `copy_cached_row_payload`, `copy_row_payload_to_output_target`, and
  `store_table_entry_cache_name_identity` were no longer visible sampled
  frames. Remaining visible storage work was the required row copy, active row
  rewrite, active row validation, table-id lookup, and journal/update setup.
- `git diff --check` passed, and the clang-format diff check for
  `packages/mylite-storage/src/storage.c` reported no formatting changes.

## Acceptance Criteria

- Cached row-payload copy helpers are inlined on the prepared point-row path.
- Table-entry cache identity refresh avoids repeated stores when the active
  pointer identity is already current.
- Existing storage and embedded storage-engine tests pass.
- Focused benchmark/profile evidence records the effect and the remaining hot
  path.

## Risks And Open Questions

- This does not remove the required copy into MariaDB's record buffer. A future
  slice would need handler-level evidence before attempting any deeper row
  materialization redesign.
