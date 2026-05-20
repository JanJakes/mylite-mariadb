# Mutable Buffer Update Rewrite

## Problem

`active-update-rewrite` safely reuses inline replacement rows only while their
replacement run is still in the active append-page buffer. The first
implementation still reads those buffered pages through the generic page-read
path and writes them back through the generic page-write path. That adds
unnecessary page copies and buffer lookups on a path whose precondition already
requires the pages to be memory-resident and mutable.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c:8070-8130` can identify whether a
  page range is fully resident in the active append-page buffer.
- `packages/mylite-storage/src/storage.c:10660-10820` used to copy the
  buffered row, row-state, and index-entry pages through `read_page_at()`,
  `read_row_state_page()`, `read_index_entry_page()`, and `write_page_at()`
  even after the buffer-residency precondition was known.
- `packages/mylite-storage/src/storage_format.h` defines fixed identity-field
  offsets for row, row-state, and index-entry pages, which are enough to verify
  that an unpublished buffered page belongs to the expected replacement run.

## Design

- Add an internal helper that returns a mutable pointer to a page inside the
  active append-page buffer when the target page is fully buffered.
- Keep the existing range-residency precondition before rewriting.
- Validate row, row-state, and index-entry page identity fields directly from
  their buffered bytes. Durable read paths still use the full page decoders and
  checksum validation.
- Encode the replacement row page directly into the buffered row page.
- Encode only changed index-entry pages directly into their buffered pages.
- Keep already-flushed replacement runs on the existing append-only path.

## Compatibility Impact

SQL and public C API behavior do not change. The change only removes redundant
copies inside the active buffered rewrite path.

## Single-File And Lifecycle Impact

No file-format, journal, WAL, lock, or companion-file change. The slice only
mutates pages that are already unpublished and resident in the active
append-page buffer.

## Tests And Verification

- Reuse the active update rewrite regression from
  `packages/mylite-storage/tests/storage_test.c`.
- Run the storage-smoke build targets, storage unit tests, the embedded
  storage-engine smoke, the update performance baseline, `git diff --check`,
  and `git clang-format --diff`.

## Verification Results

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 100000`
  measured direct primary-key updates at `15.353 us/op` and prepared
  primary-key updates at `9.650 us/op`.
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Acceptance Criteria

- Active buffered update rewrites still reuse the same row id.
- Savepoint rollback behavior remains unchanged.
- No already-flushed page is rewritten in place.
- The update baseline is not worse than the previous `active-update-rewrite`
  measurement.

## Risks And Open Questions

- This does not extend the rewrite window beyond the append buffer. Wider
  update performance still needs transient replacement-run metadata, a logged
  page-rewrite path, or a pager/WAL design.
