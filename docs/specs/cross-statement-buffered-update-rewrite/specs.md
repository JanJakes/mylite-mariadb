# Cross-Statement Buffered Update Rewrite

## Problem

The active update rewrite path can reuse a replacement row id only when the row
was created inside the current rollback frame. In a normal explicit
transaction, each direct or prepared SQL row-DML statement opens a nested
storage checkpoint. A row replacement created by an earlier successful
statement is therefore older than the current statement frame, so the update
path appends another replacement chain even when the row's pages are still in
the outer transaction append buffer.

The local update benchmark repeatedly updates 1000 rows inside one transaction.
After unchanged index-entry elision, each key-changing update still writes a
replacement row page, row-state page, and changed secondary-index entry. A
1,000,000-iteration profiling run showed pwrite-heavy append-buffer flushes as
the dominant direct-update cost and eventually hit a file-full failure before
the prepared phase completed.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/mylite-storage/src/storage.c::rewrite_active_update_pages()`
  currently rejects source row ids older than the current statement-start
  header page count. That keeps rollback simple because rollback can truncate
  pages created inside the frame.
- `append_page_buffer_statement_for_file()` returns the outermost matching
  checkpoint. Nested row-DML statements inside an explicit transaction append
  into the outer transaction buffer, while `active_statement_for_file()` returns
  the current nested statement.
- `mylite_storage_rollback_statement()` restores the statement-start
  header/catalog, flushes any retained buffered prefix before truncation, trims
  buffered pages after the restored page count, and clears parent active
  caches.
- `read_page_at()` and `write_page_at()` already read and replace pages that
  are still resident in the active append buffer.
- Full page decoders intentionally verify durable checksums. Profiling the
  first cross-statement rewrite implementation showed that re-checksumming the
  buffered row, row-state, and index pages dominated the new update loop after
  physical writes were removed.

## Design

- Add a per-statement buffered-page undo list. Each entry stores one page id
  and the page bytes as they existed before the current statement first rewrote
  that buffered page.
- Allow active update rewrite when the replacement row page, row-state page,
  and changed index-entry pages are still resident in the append buffer even if
  the source row id predates the current statement frame.
- Before rewriting a buffered page older than the current statement-start
  header page count, capture its preimage in the current statement's undo list.
  Pages created after the current statement-start page count still rely on
  truncation and do not need preimages.
- On statement rollback, restore captured buffered-page preimages before
  flushing any retained buffered prefix and before truncating. Parent cache
  invalidation remains broad enough to discard any now-stale active lookup
  state.
- On statement commit, discard the undo list. The rewritten buffered pages are
  now part of the parent checkpoint state.
- Keep the rewrite limited to buffered pages. Already-flushed pages still
  follow the append-only path until a durable WAL/page-rewrite design exists.
- Increase the transient append-buffer window enough to keep the benchmark's
  first replacement generation resident. This is still a bounded in-memory
  optimization, not a final pager.
- Decode buffered row and index rewrite candidates through checksum-free
  metadata validators. They still validate magic, page type, page id, table
  id, row id, and key size, but skip the full-page checksum scan because these
  unpublished pages were already produced and retained by MyLite in process
  memory.
- Keep a first-use checksum guard for each buffered replacement row's row-state
  page, then cache the validated row id on the append-buffer owner. Later
  rewrites of the same still-buffered row can use checksum-free row-state
  metadata validation. Statement rollback clears the parent statement chain's
  cache because page ids after the restored checkpoint can be reused. Durable
  reads and all normal decode paths keep full checksum validation.

## Affected Subsystems

- Active transaction append-page buffering.
- Statement rollback over nested row-DML checkpoints.
- Durable update publication for inline replacement rows.
- Storage unit coverage for active update rewrite and savepoint rollback.

## Compatibility Impact

SQL behavior does not change. Internal row ids may remain stable across
successful statements inside one transaction when pages are still buffered.
This is compatible because row ids are an internal storage detail.

## Single-File And Lifecycle Impact

No new durable page type or companion file is introduced. Undo pages are
transient process memory and are only used for buffered pages that have not
been flushed to the primary file.

## Public API And File-Format Impact

No public API or file-format change.

## Binary-Size And Dependency Impact

Small first-party C change. No new dependency.

## Tests And Verification

- Extend active update rewrite storage coverage so a row replacement created by
  a committed nested statement can be rewritten by a later nested statement
  while preserving the same row id.
- Verify `ROLLBACK TO SAVEPOINT` restores the previous buffered row and index
  bytes when the savepoint rewrote an older buffered replacement row.
- Verify commit persists the final rewritten row/index state and keeps old
  secondary keys hidden.
- Run storage unit tests, the embedded storage-engine smoke, the full
  storage-smoke CTest suite, update performance baseline, `git diff --check`,
  and `git clang-format --diff`.

Verification results:

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 100000`
  reported direct primary-key updates at `14.403 us/op` and prepared
  primary-key updates at `7.199 us/op`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 1000000`
  completed without the earlier file-full failure and reported direct
  primary-key updates at `13.984 us/op` and prepared primary-key updates at
  `7.284 us/op`.

## Acceptance Criteria

- Repeated updates of the same transaction-local buffered row can reuse the
  same row id across nested statement frames.
- Savepoint rollback restores the pre-savepoint row payload and secondary index
  visibility when an older buffered replacement row was rewritten.
- Already-flushed replacement rows still fall back to append-only replacement.
- Existing storage and embedded storage-engine tests remain green.
- The update benchmark no longer spends most of the measured loop in repeated
  append-buffer pwrite flushes for the 1000-row repeated-update case.

## Risks And Open Questions

- This does not solve durable page reuse after pages have flushed. It is a
  bounded stepping stone toward a WAL-backed pager.
- Larger transient append buffers reduce measured loop write pressure but move
  remaining physical writes to commit. Total transaction cost still needs a
  maintained pager/index design.
