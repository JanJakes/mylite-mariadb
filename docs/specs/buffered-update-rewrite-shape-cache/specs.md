# Buffered Update Rewrite Shape Cache

## Problem

The routed update benchmark now spends less time in catalog lookup, but sampled
prepared and direct update loops still show repeated validation work inside
`rewrite_active_update_pages()`. For rows that are updated repeatedly while
their replacement pages remain in the active append buffer, the rewrite path
keeps decoding the same row page metadata, row-state page metadata, and changed
index-entry page metadata before it can overwrite the new row payload and key
bytes.

That page shape is stable for the buffered rewrite case until a savepoint
rollback, transaction rollback, buffer flush, or changed-index shape mismatch
forces the existing fallback path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::update_row()` routes row updates to
  `mylite_storage_update_row_with_index_entry_changes()`.
- `packages/mylite-storage/src/storage.c::update_row_with_index_entries()`
  calls `rewrite_active_update_pages()` before appending a fresh replacement
  run.
- `rewrite_active_update_pages()` already restricts the rewrite path to
  active statements, inline row payloads, and row/state/index pages that are
  still present in the active append buffer.
- The existing `buffered_update_rewrites` cache proves that a row-state page has
  already passed full checksum validation, but later rewrites still decode the
  row page, row-state page, and changed index-entry pages every time.
- Nested statement rollback clears parent buffered rewrite caches through
  `clear_statement_chain_buffered_update_rewrites()`, and parent buffer range
  checks already prevent rewrite reuse after the append buffer has been flushed
  or trimmed.
- The latest one-million update sample records visible samples in
  `decode_buffered_row_page_metadata()`, `decode_buffered_row_state_page()`, and
  `decode_buffered_index_entry_page()` under `rewrite_active_update_pages()`.

## Design

- Extend the buffered update rewrite cache bucket from a row-state-only marker
  into a small row-id keyed page-shape cache.
- Cache the table id and the changed-index shape after the normal decode path
  validates the current buffered row, row-state, and changed index-entry pages.
- Store changed-index shape only for small fixed sets, using inline arrays for
  the changed index numbers and key sizes. Larger or unusual changed-index sets
  keep the existing decode path.
- On a later rewrite, if the row id, table id, changed-entry count, changed
  index numbers, and key sizes match, skip the repeated buffered page decodes
  and continue directly to undo capture plus row/key byte overwrite.
- Keep the existing decode path as the only way to populate or refresh the
  cache, so first use and shape changes still validate the buffered pages.
- Keep rollback safety through the existing parent-chain cache clearing. If the
  append buffer no longer contains the expected page range, the existing
  `buffered_append_page_range_contains()` guard prevents using the cache.

## Affected Subsystems

- MyLite storage active append-buffer rewrite state.
- Repeated row-DML update paths in active transactions and savepoints.
- Storage-smoke update performance baseline.

## Compatibility Impact

No SQL-visible or MySQL/MariaDB compatibility behavior changes. The cache only
skips repeated metadata decoding after the same buffered pages have already
passed the existing validation path under the same active storage owner.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, lock, transaction journal, recovery, or
flush lifecycle change. The cache is transient statement memory and is cleared
with statement cleanup and savepoint/rollback invalidation.

## Public API And File-Format Impact

No public `libmylite` API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency. Active storage statements keep a
bounded inline shape record per cached row id.

## Tests And Verification

- Reuse and, if needed, extend storage tests that exercise repeated active
  update rewrite, nested savepoint rollback, and large append-buffer rollback.
- Rebuild `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`.
- Run a sampled one-million-update benchmark with macOS `sample` and confirm
  the repeated decode frames under `rewrite_active_update_pages()` move down or
  disappear for the common single changed-index update path.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

Verification after implementation:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure` passed 10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`
- sampled one-million-update benchmark with macOS `sample`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`

Measured update rerun:

- Direct primary-key updates: `11.508 us/op`.
- Prepared primary-key updates: `4.765 us/op`.

Sampled rerun:

- Direct primary-key updates: `11.735 us/op`.
- Prepared primary-key updates: `4.955 us/op`.

Final verification rerun after stale-shape clearing:

- Direct primary-key updates: `11.355 us/op`.
- Prepared primary-key updates: `4.718 us/op`.

The sampled direct-update frame no longer shows repeated
`decode_buffered_row_state_page()` or `decode_buffered_index_entry_page()` work
under the common hot `rewrite_active_update_pages()` path, and only one sample
hit `decode_buffered_row_page_metadata()`. Remaining visible storage-side work
is mostly buffered undo capture, append-buffer owner/page lookup, active
indexed-row lookup/cache maintenance, and exact-index/live-row cache
maintenance.

## Acceptance Criteria

- Repeated buffered update rewrites can skip row, row-state, and small
  changed-index page decodes after the page shape is validated once.
- Shape mismatches, larger changed-index sets, rollback, statement cleanup, and
  append-buffer misses preserve the existing validated fallback path.
- Existing storage and embedded storage-engine tests remain green.
- Benchmark/profile evidence records whether the targeted decode work moved
  wall-clock update latency.

## Risks And Open Questions

- This is still an append-buffer optimization. Already-flushed replacement runs
  and cold updates use the existing append-only path.
- This does not address MariaDB parser/executor overhead or the lack of
  navigable B-tree pages. Those remain required for SQLite-like performance.
