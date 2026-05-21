# Trusted Single-Index Rewrite Change

## Problem

After the cached single-index buffered rewrite path, prepared update samples
still show `memcmp()` below `rewrite_active_single_index_update_page()`. The
fast path is entered only when the caller marked exactly one index entry as
changed and the buffered update shape cache has already validated the changed
index number and key width for the row.

For MariaDB handler calls, `index_entry_changed` is produced by
`mylite_prepare_index_entry_changes()`, which compares the old serialized key
bytes with the new serialized key bytes. The storage fast path repeats that
comparison before rewriting the already-known changed index page.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_prepare_index_entry_changes()`
  sets an index entry's changed flag from `memcmp(old_key, new_key, key_size)`.
- `packages/mylite-storage/src/storage.c::rewrite_active_update_pages()` enters
  the single-index fast path only when exactly one changed entry remains after
  the same changed bitmap is summarized.
- `buffered_update_rewrite_shape_known()` validates the cached row id, table id,
  changed index number, and key size before the fast path is used.
- The generic multi-index path still compares existing and incoming key bytes
  defensively before rewriting.

## Design

- In the cached single-index fast path, trust the already-summarized changed
  entry and rewrite the single changed index page unconditionally.
- Keep row-page undo capture before row rewrite.
- Keep index-page undo capture before index rewrite.
- Leave the generic changed-index path unchanged, including its defensive
  `memcmp()` skip.

## Scope

In scope:

- Cached one-index active append-buffer rewrites.
- Prepared update performance evidence.

Out of scope:

- Generic multi-index rewrites.
- Public storage API semantics for callers that provide conservative changed
  flags.
- MariaDB quick-range planning/execution.

## Compatibility Impact

No SQL, public C API, file-format, storage routing, or rollback semantics
change. If a caller conservatively marks an unchanged key as changed, the fast
path rewrites identical bytes after capturing undo, which is durable-state
equivalent to the previous skip.

## Single-File And Lifecycle Impact

No durable file layout, journal, lock, or companion-file lifecycle change.

## Test Plan

- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage-smoke CTest coverage.
- Run `git diff --check` and `git clang-format --diff`.
- Run `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`.
- Sample the prepared update phase and confirm the cached single-index fast path
  no longer spends visible time in its own key `memcmp()`.

## Acceptance Criteria

- Cached one-index rewrites no longer compare the existing and incoming key
  bytes before rewriting.
- Index-page undo is still captured before the index page is rewritten.
- Generic changed-index rewrites keep their existing comparison.
- Storage and embedded routed-engine tests pass.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c`: no
  changes.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key updates measured
  `2.382 us/op`; the sampled run measured `2.462 us/op`.
- A two-second macOS `sample` run no longer showed `_platform_memcmp` under
  `rewrite_active_single_index_update_page()`. Remaining storage-side cost in
  that helper is row/index page copy plus buffered undo capture.
