# Buffered Update Row-State Cache

## Problem

Cross-statement buffered update rewrite keeps a first-use checksum guard for a
replacement row's row-state page, then caches the validated row id on the
append-buffer owner. The first implementation stores those row ids in a growable
array and scans it linearly on every later rewrite.

The local one-million-update benchmark now spends measurable CPU in that scan
after physical append writes have been removed from the hot loop. With 1000 hot
rows, each update can probe a list that grows to 1000 row ids before it decides
whether the row-state page already had its full checksum guard.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The benchmark path enters MariaDB through
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` and then calls
  `mylite_storage_update_row_with_index_entry_changes()`.
- `packages/mylite-storage/src/storage.c::rewrite_active_update_pages()` is the
  storage hot path once a replacement row's page run is still resident in the
  active append buffer.
- `buffered_update_rewrite_row_state_known()` is currently an O(n) scan over
  the append-buffer owner's validated row ids.
- The cache is an optimization only. If marking a row id fails, later updates
  can keep running the full row-state checksum guard without changing SQL
  behavior.
- The cache has no per-entry delete operation. Statement cleanup and rollback
  clear the whole cache, so open addressing without tombstones is sufficient.

## Design

- Replace the growable row-id array with a small open-addressed row-id set.
- Use the existing `hash_row_id()` helper and power-of-two bucket capacity.
- Grow the set before insertion when the next count would exceed a 50% load
  factor, starting with a small bucket table.
- Keep lookup side-effect free and allocation free.
- Keep `mark_buffered_update_rewrite_row_state()` best-effort at its caller.
  Allocation failure means the row id is not marked and the next rewrite will
  run the full checksum guard again.
- Keep rollback invalidation unchanged: rolling back a nested statement clears
  the parent statement chain's row-state validation caches.

## Affected Subsystems

- MyLite storage active update rewrite.
- Statement-owned transient validation caches.
- Storage-smoke performance baseline.

## Compatibility Impact

No SQL, handler, or public API behavior changes. This is an internal cache data
structure change for a validation shortcut.

## Single-File And Lifecycle Impact

No durable file-format or companion-file change. The cache remains transient
process memory owned by active statement checkpoints.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- Run the storage unit test and full storage-smoke CTest gate.
- Run the update performance baseline with `--phase=updates 1000 1000000` and
  confirm `buffered_update_rewrite_row_state_known()` no longer appears as a
  material sampled hot spot.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

Verification results:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 1000000`
  reported direct primary-key updates at `13.435 us/op` and prepared
  primary-key updates at `6.700 us/op`.
- A sampled one-million-update run showed
  `buffered_update_rewrite_row_state_known()` dropping from roughly 190 samples
  in the prior profile to 5 samples after the hash-backed cache change.

## Acceptance Criteria

- Cross-statement buffered update rewrite behavior remains unchanged.
- Savepoint rollback still clears parent-chain validation caches.
- Repeated updates over 1000 hot rows no longer linearly scan the validated
  row-state cache.
- Existing storage and embedded storage-engine tests remain green.

## Risks And Open Questions

- This does not address the larger checksum-generation cost for rewritten row
  and index pages. That remains the next storage CPU target if this slice lands
  cleanly.
