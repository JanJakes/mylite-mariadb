# Checkpoint Tail Page Truncation

## Problem

MyLite checkpoints restore logical visibility by writing the saved header and,
when needed, catalog root page back to the primary `.mylite` file. That restores
`page_count`, so later scans ignore pages appended by a rolled-back statement,
savepoint, transaction, or pending recovery journal. The file can still keep
those bytes past the restored `page_count`, though, which makes failed work grow
the primary file until a later overwrite or full compaction slice.

This slice makes rollback and journal recovery truncate the physical primary
file to the header's published `page_count`. It is a tail-only reclamation step,
not general compaction.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB statement transaction boundaries call handler commit/rollback through
  `ha_commit_trans(thd, FALSE)` and `ha_rollback_trans(thd, FALSE)` in
  `mariadb/sql/transaction.cc:476-582`.
- MyLite registers handler transaction and savepoint hooks in
  `mariadb/storage/mylite/ha_mylite.cc:571-588`.
- MyLite maps handler statement, savepoint, and transaction rollback paths to
  `mylite_storage_rollback_statement()` in
  `mariadb/storage/mylite/ha_mylite.cc:604-758` and
  `mariadb/storage/mylite/ha_mylite.cc:2983-3176`.
- Storage checkpoint rollback restores saved catalog/header pages, optionally
  republishes advancing autoincrement pages, truncates to the final header page
  count, flushes, then closes the checkpoint in
  `packages/mylite-storage/src/storage.c:3117-3168`.
- Pending recovery-journal restore writes the saved pages, truncates to the
  saved header page count, and flushes the file in
  `packages/mylite-storage/src/storage.c:3702-3778`.
- The storage header already exposes `page_count`, and all page scans bound
  themselves to that value. Pages physically beyond that value are not part of
  the logical file image.

## Design

- Add a first-party storage helper that truncates an opened primary file to
  `header.page_count * header.page_size`.
- Call the helper after checkpoint rollback has restored the header/catalog and
  after any rollback-preserved autoincrement pages have been republished.
- Call the helper after pending recovery-journal restore writes the saved
  header/catalog pages.
- Flush after truncation so the restored file length participates in the same
  durability boundary as the restored pages.
- Treat integer overflow or offsets beyond `LONG_MAX` as unsupported, matching
  the existing page seek limits.

## Non-Goals

- Do not introduce a reusable free list.
- Do not reuse row page ids. Published row-state pages can refer to row page
  ids, so page reuse needs a real compaction design.
- Do not reclaim orphaned row, overflow, index-entry, or autoincrement pages
  from committed update/delete/truncate history.
- Do not change catalog, row, index, or autoincrement page formats.

## Compatibility Impact

SQL and C API behavior is unchanged. The visible difference is file lifecycle:
rolled-back tail pages no longer remain in the primary file after checkpoint
rollback or journal recovery. This strengthens the single-file invariant
without claiming general compaction support.

## File-Format Impact

No format-version bump is needed. The slice uses the existing header
`page_count` as the authoritative physical tail after rollback or recovery.
`free_list_root_page` remains zero and unused.

## Test Plan

- Add storage tests proving:
  - a statement rollback after appended row and index pages restores the file
    length to the checkpoint header page count;
  - rollback-preserved autoincrement pages remain durable and the file length
    matches the republished header page count;
  - recovery from row-publication and catalog-publication journals truncates
    the primary file back to the saved header page count.
- Run the first-party storage test target.
- Run the compatibility harness groups that cover storage core, statement
  rollback, crash recovery, and transaction hooks.
- Run static script syntax checks and `git diff --check`.

## Acceptance Criteria

- No storage reads observe rolled-back tail pages.
- File size equals `header.page_count * header.page_size` after checkpoint
  rollback and recovery-journal restore.
- Existing autoincrement rollback-preservation semantics remain unchanged.
- Documentation continues to state that general committed-page compaction and
  free-space reuse remain planned.
