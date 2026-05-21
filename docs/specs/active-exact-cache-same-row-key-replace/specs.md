# Active Exact-Cache Same-Row Key Replace

## Problem

Prepared primary-key updates commonly rewrite an active buffered replacement
row in place. When the logical row id is unchanged but one secondary key image
changes, active exact-index cache maintenance still removes every cached entry
for that row id and appends a replacement entry.

That remove-plus-append path is correct, but it does more work than needed for
the hot same-row case: it updates row-id buckets, key buckets, live/dead entry
counts, and may compact cache arrays even though the cache entry can keep the
same row id and entry slot.

## Source Findings

- Base line: MariaDB 11.8.6, import ref
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- This slice is limited to first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`.
- `update_row_with_index_entries()` sets `position.row_page_id = row_id` when
  `rewrite_active_update_pages()` rewrites the active append-buffer row in
  place.
- `replace_active_exact_index_cache_entries_in_statement()` already skips
  unchanged cached index entries for same-row updates, but changed cached
  entries still use `remove_exact_index_cache_entries_by_row_id()` followed by
  `append_exact_index_cache_entry()`.
- `mylite_storage_exact_index_cache` has separate key buckets and row-id
  buckets. The changed same-row case only needs to move one live entry between
  key buckets; the row-id bucket can keep the same row id.

## Design

- Add a private helper that replaces the key bytes for a live exact-index cache
  entry with a matching row id.
- When key buckets are valid, unlink the entry from its old key bucket, copy
  the new key bytes into the same slot, and link it into the new key bucket.
- When row-id buckets are valid, use them to locate the row-id entry and remove
  any duplicate live entries for the same row id, preserving the existing
  remove-plus-append semantics of at most one cached entry per row id in one
  index cache.
- If no cached entry exists for the row id, append the new entry, matching the
  current behavior.
- Keep the existing remove-plus-append path for row-id-changing updates,
  deletes, unknown row-id bucket state, and generic fallback cases.

## Scope

In scope:

- Active exact-index cache maintenance for successful same-row durable updates.
- The existing changed-index vector produced by MyLite storage callers.
- Storage-smoke performance evidence for prepared primary-key updates.

Out of scope:

- Durable exact-index cache maintenance.
- SQL-layer quick planning or direct update pushdown.
- File format, journal format, recovery semantics, or public API changes.

## Compatibility Impact

No SQL, handler, public C API, or storage-engine routing behavior changes. The
same active exact-index cache entries remain visible after successful updates,
and cache miss or allocation failure keeps the existing conservative fallback.

## Single-File And Lifecycle Impact

No durable file, lock, journal, recovery, or companion-file lifecycle changes.
The affected cache is transient active-checkpoint memory.

## Binary-Size And Dependency Impact

Small first-party C helper. No new dependencies.

## Test And Verification Plan

- Make the active exact-index cache replacement test assert that the second
  update preserves the active row id while changing the cached secondary key.
- Build `mylite_storage_test`, `mylite_embedded_storage_engine_test`, and
  `mylite_perf_baseline`.
- Run focused storage and embedded storage-engine CTest coverage.
- Run `git diff --check` and `git clang-format --diff` on touched C files.
- Run the prepared-update performance baseline and sample it when useful.

## Acceptance Criteria

- Same-row active exact-cache updates change the cached key in place when the
  row-id entry already exists.
- Row-id-changing updates, deletes, and fallback paths keep current behavior.
- Existing exact-index cache tests pass, including replacement, delete, commit,
  and reopened read assertions.
- Focused builds and routed storage tests pass.

## Risks

- Exact-cache bucket maintenance is subtle. The helper must update key buckets
  only after unlinking the old key and must leave row-id buckets consistent
  when duplicate cached entries are removed.
- If row-id bucket construction fails, the implementation should use the
  existing remove-plus-append fallback rather than weakening correctness.

## Verification Evidence

- `git diff --check`: passed.
- `git clang-format --diff -- packages/mylite-storage/src/storage.c
  packages/mylite-storage/tests/storage_test.c`: no changes.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test
  mylite_embedded_storage_engine_test mylite_perf_baseline`: passed.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R
  'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed.
- `ctest --preset storage-smoke-dev --output-on-failure`: 10/10 passed.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key updates
  measured `2.242 us/op`; the sampled run measured `2.324 us/op`.
- A two-second macOS `sample` run showed the old
  `replace_active_exact_index_cache_entries_in_statement()` frame down to a
  small residual frame; `rewrite_active_single_index_update_page()` and mixed
  MariaDB planning/execution frames are now more prominent.
