# Direct Buffered Update Rewrite

## Problem

After the cross-statement buffered update rewrite and row-state validation
cache, repeated primary-key updates no longer append a new replacement page
chain for every statement while the replacement run is still in the active
append buffer. The active rewrite path still reads the buffered row and index
pages through `read_page_at()` and writes replacements through `write_page_at()`.
Those calls route through `copy_buffered_append_page()` and
`replace_buffered_append_page()`, so the hot loop still performs avoidable
4096-byte copies for pages that are already mutable in process memory.

The local one-million-update profile after span-based checksum hashing showed
those buffered page copy helpers as a remaining update-path cost.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The hot SQL path enters MariaDB through
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` and calls
  `mylite_storage_update_row_with_index_entry_changes()` for durable routed
  tables.
- `packages/mylite-storage/src/storage.c::rewrite_active_update_pages()`
  handles repeated updates of still-buffered inline replacement rows.
- `read_page_at()` consults `copy_buffered_append_page()` before physical file
  reads, and `write_page_at()` consults `replace_buffered_append_page()` before
  physical file writes.
- `capture_buffered_page_undo()` must keep a full preimage of each older
  buffered page the current statement mutates, so rollback can restore pages
  that cannot be handled by truncating to the statement-start page count.
- The existing checksum-free buffered row and index metadata decoders are
  intentionally limited to unpublished in-memory pages; durable decode paths
  still validate full page checksums.

## Design

- Add a private `buffered_append_page()` helper that returns a mutable pointer
  to a page resident in the active append buffer when the page id and page size
  are within the current buffered range.
- Keep `copy_buffered_append_page()` and `replace_buffered_append_page()` as
  copy-based wrappers around the pointer helper for generic read/write callers.
- In `rewrite_active_update_pages()`, validate the current row page, row-state
  page, and changed index-entry pages directly from append-buffer pointers
  instead of first copying them to stack or heap page buffers.
- Preserve the first-use row-state checksum guard. Later rewrites of the same
  validated buffered row may keep using checksum-free row-state metadata
  validation until rollback or statement cleanup clears the cache.
- Capture the same full-page undo preimages before mutating older buffered
  pages, then encode the replacement row and changed index-entry pages directly
  into the append buffer.
- Keep already-flushed replacement pages on the append-only path. Direct
  mutation is only for transient unpublished pages owned by the active
  checkpoint.

## Affected Subsystems

- MyLite active append-page buffer.
- Statement rollback over nested row-DML checkpoints.
- Durable update publication for inline replacement rows.
- Storage-smoke update performance baseline.

## Compatibility Impact

SQL, handler, public API, and MySQL/MariaDB compatibility behavior do not
change. Internal row ids may continue to stay stable across repeated updates of
still-buffered replacement rows, as specified by the earlier cross-statement
rewrite slice.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, or lifecycle change. The direct pointer
is only used for pages still held in the transient append buffer. Durable file
reads and already-flushed page runs keep their existing paths.

## Public API And File-Format Impact

No public API or file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- Run the storage unit test and full storage-smoke CTest gate.
- Run the update performance baseline with `--phase=updates 1000 1000000`.
- Sample the same update baseline to confirm `copy_buffered_append_page()` and
  `replace_buffered_append_page()` are no longer material hot spots.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

Verification results:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 1000000`
  reported direct primary-key updates at `12.921 us/op` and prepared
  primary-key updates at `7.329 us/op` in the final verification run.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 1000000`
  reported direct primary-key updates at `12.668 us/op` and prepared
  primary-key updates at `6.298 us/op` in the initial local run.
- A sampled one-million-update run reported direct primary-key updates at
  `12.838 us/op` and prepared primary-key updates at `6.102 us/op`.
  `copy_buffered_append_page()` and `replace_buffered_append_page()` were no
  longer material hot spots; remaining measured costs included
  `checksum_page_zero_tail()`, `capture_buffered_page_undo()`, and
  `mylite_check_duplicate_keys()`.

## Acceptance Criteria

- Repeated updates of still-buffered replacement rows preserve existing row-id
  reuse and savepoint rollback behavior.
- Existing storage and embedded storage-engine tests remain green.
- Full-page preimage rollback still protects older buffered pages mutated by a
  nested statement.
- The update profile no longer spends meaningful time copying row and index
  pages between the append buffer and temporary page buffers.

## Risks And Open Questions

- This does not remove the full-page undo copies needed for nested statement
  rollback. A narrower undo format would need its own correctness design.
- Remaining update cost is expected to shift toward checksum generation,
  duplicate-key checks, and undo capture until the project has a real pager and
  maintained navigable indexes.
