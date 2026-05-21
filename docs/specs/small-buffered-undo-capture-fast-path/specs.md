# Small Buffered Undo Capture Fast Path

## Problem

Prepared point updates usually capture a very small active buffered-page undo
set for the nested statement. The hot row-only rewrite path often starts with an
empty undo list, but `capture_buffered_page_undo_from_page()` still calls the
duplicate lookup helper and bucket-capacity helper before appending the first
undo entry.

Sampling shows `capture_buffered_page_undo_from_page()` in the remaining
first-party prepared-update stack.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), with this slice limited to
  first-party MyLite storage code.
- `rewrite_active_row_only_update_page()` captures one buffered row-page undo
  before rewriting the active append buffer.
- Buffered-page undo buckets are deliberately not created until
  `MYLITE_STORAGE_BUFFERED_PAGE_UNDO_BUCKET_MIN_COUNT`.
- Reusable nested statements keep a small undo entry array attached, so ordinary
  prepared point updates usually append into existing entry capacity.

## Design

- Use a local `undos` pointer inside `capture_buffered_page_undo_from_page()`
  to avoid repeated member access.
- Skip duplicate lookup when `undos->count == 0`.
- Call `ensure_buffered_page_undo_buckets()` only when buckets already exist or
  the next append reaches the bucket minimum.
- Keep duplicate protection, capacity growth, bucket insertion, partial-page
  undo copy, and checksum-dirty preservation unchanged.

## Compatibility Impact

No SQL, public C API, storage-engine routing, file-format, or durability
behavior changes. Statement rollback captures the same page preimages.

## Single-File And Lifecycle Impact

No durable file, journal, lock, recovery, or companion-file lifecycle changes.
The change only avoids helper work before appending transient statement undo
entries.

## Tests And Verification

- Run:
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
- Run prepared-update timing only when unrelated machine load is low.

## Acceptance Criteria

- Empty small buffered-page undo captures append without duplicate lookup or
  bucket ensure calls.
- Existing active rewrite, statement rollback, transaction rollback, and
  embedded storage-engine tests pass.

## Risks

- Duplicate protection must remain for non-empty undo lists. The fast path only
  skips duplicate lookup when there cannot be a prior entry.
