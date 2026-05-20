# Buffered Rewrite Page Refs

## Problem

Prepared primary-key updates inside an active checkpoint now rewrite
unpublished row and changed index-entry pages directly in the append buffer.
The current active rewrite path already resolves the append-buffer owner once,
but it still rediscovers each page's checksum-dirty slot after validating or
capturing the same buffered page.

The sampled prepared-update profile before this slice still showed storage-side
samples in `rewrite_active_update_pages()`,
`buffered_append_page_in_statement()`, undo capture, and dirty-slot lookup
helpers.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MyLite-owned source: `packages/mylite-storage/src/storage.c`.
- `rewrite_active_update_pages()` validates the active buffered row page and
  changed index-entry pages, captures rollback preimages, mutates those pages,
  and marks their checksums dirty.
- `buffered_append_page_in_statement()` and
  `buffered_append_page_checksum_dirty_slot_in_statement()` perform the same
  statement-local page-range checks for the same page ids.
- The undo-capture helper read the buffered page and its dirty state after the
  active rewrite caller had already resolved the page.

## Design

- Add a small private `mylite_storage_buffered_page_ref` carrying both the
  append-buffer page pointer and its checksum-dirty slot.
- Add one statement-scoped page-ref resolver that performs the existing
  page-size, buffer-presence, dirty-array, and page-range checks once.
- Keep the existing page and dirty-slot helpers as wrappers for generic callers.
- Route `rewrite_active_update_pages()` through page refs for the current row
  page and changed index-entry pages.
- Capture rollback preimages from the already-resolved page ref, preserving the
  same duplicate-undo, allocation, used-prefix, and dirty-checksum behavior.
- Mark dirty checksum slots directly through the page ref after mutation.

## Compatibility Impact

No SQL, handler, public API, storage-engine routing, or MySQL/MariaDB
compatibility behavior changes.

## File Lifecycle

No durable file-format, journal, lock, recovery, or companion-file lifecycle
change. Page refs point only at transient unpublished append-buffer pages.
Flush and generic read paths still refresh dirty checksums before pages leave
the buffer or enter checksum-validating decode paths.

## Embedded Lifecycle And API

No public `libmylite` API or embedded lifetime change.

## Build, Size, And Dependencies

Small first-party C helper refactor. No new dependency and no expected material
binary-size impact.

## Test Plan

- Build storage-smoke targets with `cmake --build --preset storage-smoke-dev`.
- Run `git diff --check`.
- Run `ctest --test-dir build/storage-smoke-dev --output-on-failure`.
- Run `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`.

## Acceptance Criteria

- Active buffered update rewrites do not rediscover dirty slots for pages they
  have already resolved.
- Savepoint rollback still restores page bytes and checksum-dirty state.
- Existing storage and embedded storage-engine coverage remains green.
- Prepared-update timing does not materially regress.

## Verification Evidence

- `cmake --build --preset storage-smoke-dev`: passed.
- `git diff --check`: passed.
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`: passed,
  10/10.
- `build/storage-smoke-dev/tools/mylite_perf_baseline
  --phase=prepared-updates 1000 1000000`: prepared primary-key update repeats
  measured `4.316 us/op` and `4.352 us/op` after the final helper shape.

## Risks And Open Questions

- This only removes repeated in-memory lookup work. SQLite-like write
  throughput still requires the larger navigable-index and pager/WAL roadmap
  work.
