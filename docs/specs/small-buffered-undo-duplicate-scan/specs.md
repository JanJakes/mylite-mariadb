# Small Buffered Undo Duplicate Scan

## Problem

Prepared primary-key updates that change one secondary key capture two buffered
page undo preimages in the nested statement: the row page and the changed index
entry page. After the first capture, `capture_buffered_page_undo_from_page()`
still calls `find_buffered_page_undo()` to scan a one-entry undo list before
appending the second preimage.

Sampling shows `find_buffered_page_undo()` and
`capture_buffered_page_undo_from_page()` in the remaining prepared-update
storage stack.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `rewrite_active_update_pages()` captures the active row page before rewriting
  it, then captures each changed index-entry page before rewriting changed key
  bytes.
- Buffered-page undo buckets are intentionally not created until
  `MYLITE_STORAGE_BUFFERED_PAGE_UNDO_BUCKET_MIN_COUNT`.
- For lists below that threshold, duplicate detection is a linear scan over the
  small append-only undo entry array.

## Design

- Keep the existing empty-list fast path.
- When buffered-page undo buckets do not exist, scan the small undo entry array
  directly inside `capture_buffered_page_undo_from_page()`.
- Use `find_buffered_page_undo()` only when bucketed lookup exists.
- Preserve duplicate protection, capacity growth, bucket insertion, rollback
  order, and undo preimage bytes.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. The same page ids are deduplicated before capture.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only avoids an extra helper call for transient small statement undo
lists.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- Run prepared-update timing only when unrelated machine load is low.

## Acceptance Criteria

- Small non-bucketed buffered-page undo lists deduplicate without calling
  `find_buffered_page_undo()`.
- Bucketed undo lists keep the existing lookup path.
- Existing active rewrite, statement rollback, transaction rollback, and
  embedded storage-engine tests pass.

## Risks

- Duplicate protection must remain exact for small undo lists. The direct scan
  checks the same `page_id` field as the helper's non-bucketed path before any
  append can occur.
