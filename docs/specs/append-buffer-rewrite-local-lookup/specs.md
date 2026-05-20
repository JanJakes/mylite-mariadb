# Append Buffer Rewrite Local Lookup

## Problem

After the buffered update rewrite shape cache, the sampled update profile shows
the repeated decode frames moving down, but the hot rewrite path still spends
visible time rediscovering the active append-buffer owner from `FILE *` inside
small helpers. `rewrite_active_update_pages()` already resolves the active
buffer statement at entry, but calls helpers such as `buffered_append_page()`,
`buffered_append_page_range_contains()`, `capture_buffered_page_undo()`, and
dirty-flag setters that each call `append_page_buffer_statement_for_file()`
again.

Those repeated owner lookups are redundant inside a single rewrite attempt.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::rewrite_active_update_pages()` calls
  `active_statement_for_file()` and `append_page_buffer_statement_for_file()` at
  entry.
- The same function then repeatedly reaches buffered pages through helpers that
  rediscover the append-buffer statement by scanning active statements.
- The latest sampled one-million update profile records
  `append_page_buffer_statement_for_file()` below `rewrite_active_update_pages()`
  and `capture_buffered_page_undo()`, after the repeated buffered decode work
  was removed.

## Design

- Add internal append-buffer helper variants that accept the already-resolved
  `mylite_storage_statement *buffer_statement`.
- Route the hot `rewrite_active_update_pages()` path through those local helper
  variants for range checks, page lookup, undo capture, and dirty-flag updates.
- Keep the existing `FILE *` helper wrappers for all other call sites so the
  broader storage API behavior does not change.
- Preserve all page bounds, page-size, empty-buffer, and checksum-dirty
  semantics in one shared implementation where practical.

## Affected Subsystems

- MyLite storage active append-buffer rewrite path.
- Storage-smoke update performance baseline.

## Compatibility Impact

No SQL or MySQL/MariaDB compatibility behavior changes. This is an internal
control-plane lookup optimization.

## Single-File And Lifecycle Impact

No durable file-format, companion-file, lock, transaction, journal, recovery, or
flush lifecycle change.

## Public API And File-Format Impact

No public `libmylite` API, internal storage API, or file-format change.

## Binary-Size And Dependency Impact

Small first-party C helper refactor. No new dependency.

## Tests And Verification

- Rebuild `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline` with the `storage-smoke-dev` preset.
- Run `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000`.
- Run a sampled one-million-update benchmark with macOS `sample` and confirm
  append-buffer statement lookup samples move down under the hot rewrite path.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

## Acceptance Criteria

- `rewrite_active_update_pages()` avoids rediscovering the same append-buffer
  statement through `FILE *` helper calls.
- Existing append-buffer helper behavior is preserved for other callers.
- Existing storage and embedded storage-engine tests remain green.
- Benchmark/profile evidence records whether the local lookup refactor moves
  update latency or profile shape.

## Verification Evidence

- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000
  1000000` measured one run at `10.977 us/op` direct primary-key updates and
  `4.643 us/op` prepared primary-key updates.
- A five-second macOS `sample` run recorded only one
  `append_page_buffer_statement_for_file()` sample below
  `rewrite_active_update_pages()`, with the repeated buffered-page accesses
  moving through `buffered_append_page_in_statement()`.

## Risks And Open Questions

- This is intentionally small and does not change the append-buffer data
  structure. The remaining path may still need a row-id-to-buffer-offset index
  or undo lookup cache if page and undo searches become dominant.
