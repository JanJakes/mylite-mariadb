# Buffered Index Leaf Checksums

## Problem

Maintained branch-index inserts can rewrite an existing index leaf and its
branch fence inside the same active checkpoint. The dirty-page buffer already
coalesces maintained root and branch rewrites and refreshes checksum-dirty pages
when a generic read copies them or when the checkpoint flushes them. Existing
index leaf rewrites still refresh the leaf checksum eagerly before buffering or
writing the page, so repeated writes to the same leaf pay checksum cost before
the final checkpoint publication needs it.

Recent prepared-insert profiling still shows checksum work in branch-index
maintenance. Full branch refold snapshots remain the dominant cost, but the
simple maintained leaf rewrite path can use the same checksum-dirty buffering
contract as maintained branch pages.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB handler writes are still statement-level row mutations:
  `mariadb/sql/handler.cc` dispatches `handler::ha_write_row()` to the engine's
  `write_row()` implementation, and `mariadb/storage/mylite/ha_mylite.cc`
  forwards routed durable writes to MyLite storage.
- This slice changes first-party MyLite storage only:
  `packages/mylite-storage/src/storage.c` and
  `packages/mylite-storage/tests/storage_test.c`.
- `pager_write_maintained_insert_page()` can already accept a
  `checksum_dirty` page and either refresh it before a direct write or route it
  into the dirty-page buffer.
- `buffer_dirty_page_for_pager_write()` currently accepts only maintained index
  root and branch pages, then refreshes only branch cache metadata.
- `refresh_dirty_buffered_page_checksum()` can refresh row, index-entry,
  maintained-root, and branch pages, but not index-leaf pages.
- `copy_dirty_page_buffer()` refreshes a checksum-dirty copy for generic reads
  without clearing the buffered entry's dirty marker; the checkpoint flush path
  refreshes the stored page before writing it to the primary file.
- Active index leaf cache metadata can be populated from trusted page bytes
  without verifying the checksum, which lets active branch planning keep using
  a checksum-dirty leaf safely.

## Design

Extend the maintained dirty-page buffering contract to index leaf pages:

- accept maintained index leaf pages in the dirty-page buffer;
- refresh index leaf checksums in `refresh_dirty_buffered_page_checksum()` using
  the leaf `used_bytes` field;
- update both active leaf and active branch caches after a buffered maintained
  page write, according to the actual page type;
- leave generic read and flush semantics unchanged; and
- mark the simple branch leaf insert rewrite as checksum-dirty so the leaf page
  is checksummed only when copied for a generic read or flushed.

The slice deliberately keeps snapshot leaf-run publication unchanged. Those
pages are currently encoded as complete durable pages and written as a direct
run; deferring those checksums through append buffering needs a separate design.

## Non-Goals

- No file-format change.
- No change to branch refold selection or live-tail overlay handling.
- No deferral for freshly appended branch snapshot leaf runs.
- No change to MariaDB handler semantics, SQL behavior, or storage-engine
  routing.

## Compatibility Impact

No user-visible SQL, C API, handler, wire-protocol, or metadata behavior
changes. Durable index leaf bytes written to the primary `.mylite` file remain
checksummed before publication.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Checksum-dirty leaf pages
are statement-local dirty-buffer entries protected by the existing journal and
flushed through the existing checkpoint lifecycle.

## Build, Size, And Dependencies

Small first-party C test and storage change only. No new dependency or embedded
profile change.

## Test Plan

- Add storage test-hook coverage proving checksum-dirty index leaf pages:
  - stay marked dirty in the buffer;
  - are checksummed in generic read copies;
  - are checksummed before dirty-buffer flush; and
  - update active leaf cache metadata without checksum validation.
- Run storage unit coverage.
- Run storage-smoke embedded storage-engine coverage.
- Run the prepared insert component benchmark as local performance evidence.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

## Acceptance Criteria

- Dirty-page buffering accepts maintained index leaf pages.
- Checksum-dirty leaf pages never reach disk with a zero or stale checksum.
- Active branch maintenance can read freshly buffered leaves through the active
  leaf cache.
- Existing storage and routed storage-engine tests pass.

## Risks And Open Questions

- The dominant refold snapshot leaf encoding cost remains until snapshot
  publication can safely defer appended leaf checksums or avoid refolds.
- Buffered leaf pages rely on the same statement rollback and recovery journal
  protection as other dirty existing pages; tests should exercise the common
  buffer copy and flush paths, while broader rollback coverage remains inherited
  from dirty-page tests.

## Verification Results

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed: 1/1 test, 155.15 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  passed: 1/1 test, 42.47 seconds.
- Four sequential runs of
  `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  measured `prepared insert step component` at `20.248`, `19.761`, `20.080`,
  and `20.215 us/op`. The benchmark emitted the known CSV-engine fallback
  message.
