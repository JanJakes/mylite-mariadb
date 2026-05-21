# Active Update Rewrite Hot Probes

## Problem

Prepared primary-key update profiling now shows MyLite-owned active update
rewrite helpers as visible storage frames. The main sampled functions are the
buffered update-rewrite shape probe, its row-id hash lookup, and compact
row/index undo-size helpers used before capturing rollback preimages.

These functions are small control helpers around the already-implemented active
rewrite path. They do not own SQL semantics or file-format decisions, but they
sit inside every hot prepared row update that can rewrite buffered row and
index-entry pages in place.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `ha_mylite::update_row()` calls MyLite storage update APIs from
  `mariadb/storage/mylite/ha_mylite.cc`.
- Durable row updates route through `update_row_with_index_entries()` and then
  `rewrite_active_update_pages()` in `packages/mylite-storage/src/storage.c`.
- `rewrite_active_update_pages()` probes cached rewrite shape with
  `buffered_update_rewrite_shape_known()` before choosing the single-index
  rewrite path.
- The shape probe calls `find_buffered_update_rewrite_bucket()`, which hashes
  row ids through `hash_row_id()`.
- `rewrite_active_single_index_update_page()` captures compact row and index
  preimages using `buffered_row_page_undo_used_size()` and
  `buffered_index_entry_page_undo_used_size()` before mutating buffered pages.

## Design

- Mark buffered update-rewrite shape and row-state probe helpers as
  `MYLITE_STORAGE_HOT_INLINE`.
- Mark the buffered update-rewrite bucket lookup and row-id hash helper as
  `MYLITE_STORAGE_HOT_INLINE`.
- Mark compact row/index undo-size helpers as `MYLITE_STORAGE_HOT_INLINE`.
- Keep the existing probing, collision handling, shape validation, undo capture,
  and rewrite behavior unchanged.

## Affected Subsystems

- MyLite active row update rewrite path.
- MyLite buffered update-rewrite cache.
- MyLite statement rollback preimage capture.
- Prepared primary-key update performance baseline.

## Compatibility Impact

No SQL, public API, handler API, storage-engine routing, metadata, or
file-format behavior change. The same pages are rewritten, the same undo bytes
are captured, and the same fallback paths remain.

## Single-File And Lifecycle Impact

No durable storage, journal, lock, or companion-file lifecycle change. This
slice changes only transient helper call shape on active statements.

## Public API And File-Format Impact

No public API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party code-layout change. No dependency change. The inlined helpers
are short and already storage-local.

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
  `buffered_update_rewrite_shape_known`,
  `find_buffered_update_rewrite_bucket`, `hash_row_id`,
  `buffered_row_page_undo_used_size`, and
  `buffered_index_entry_page_undo_used_size` remain visible frames.
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
  at 2.275 us/op.
- The focused sampled run recorded 2.331 us/op in
  `/tmp/mylite-active-update-rewrite-hot-probes.sample`.
  `buffered_update_rewrite_shape_known`,
  `find_buffered_update_rewrite_bucket`, `hash_row_id`,
  `buffered_row_page_undo_used_size`, and
  `buffered_index_entry_page_undo_used_size` were no longer visible sampled
  frames. Remaining visible storage work was active single-index rewrite,
  undo-entry copy/initialization, table-id lookup, live-row validation, and
  handler-side key preparation.
- `git diff --check` passed, and the clang-format diff check for
  `packages/mylite-storage/src/storage.c` reported no formatting changes.

## Acceptance Criteria

- Buffered update-rewrite probes and compact undo-size helpers are inlined on
  the prepared update rewrite path.
- Existing rollback preimage capture and active rewrite behavior stay covered
  by the storage and embedded storage-engine tests.
- Focused benchmark/profile evidence records the effect and remaining hot path.

## Risks And Open Questions

- This does not change the amount of undo data copied. A future slice should
  inspect whether same-size row/index rewrites can avoid some preimage work
  without weakening statement rollback.
