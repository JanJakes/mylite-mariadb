# Active Update Rewrite

## Problem

The current routed update path is append-only. Every successful update writes a
replacement row page, a row-state page, and one index-entry page for each
index. In the local update benchmark, rows are updated repeatedly inside one
transaction, so the file grows by another full replacement chain even when the
row being updated was itself created by an earlier uncommitted replacement.

Recent profiling after cache-maintenance work shows physical append writes as
the dominant remaining update-path cost. Increasing the append buffer beyond
the proven 1024-page window is still unsafe: a scratch 2048-page retry passed
two embedded storage-engine repeats and then hit a bus error on the third.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc:2350-2385` starts a durable transaction
  checkpoint for explicit SQL transactions. Ordinary row DML inside that
  transaction writes into the active storage statement rather than opening a
  separate durable checkpoint per row statement.
- `mariadb/storage/mylite/ha_mylite.cc:653-702` creates explicit savepoint
  frames by calling `mylite_storage_begin_statement()`.
- `mariadb/storage/mylite/ha_mylite.cc:2623-2770` calls
  `mylite_storage_update_row_with_index_entries()` after duplicate-key and FK
  checks.
- `packages/mylite-storage/src/storage.c:4483-4638` appends replacement row,
  row-state, and index-entry pages for every update, then updates active
  caches.
- `packages/mylite-storage/src/storage.c:5393-5520` rolls back an active
  statement by restoring the statement-start header/catalog and truncating to
  that header page count.
- Because explicit savepoints are represented as nested storage statements, a
  row page with `row_id >= active_statement->header.page_count` was created
  inside the current rollback frame. Rewriting it is rollback-safe: rollback
  truncates that row page and any associated index pages.

## Design

- Before appending a replacement chain, try an active rewrite fast path.
- The fast path applies only when:
  - an active storage statement owns the file;
  - the source `row_id` is greater than or equal to that active statement's
    statement-start `header.page_count`;
  - the current source row page is inline and has no overflow payload pages;
  - the row payload fits the inline row-page format;
  - the replacement row page, row-state page, and index-entry pages are still
    resident in the active append-page buffer;
  - page `row_id + 1` is a replacement row-state page whose replacement row id
    is `row_id`;
  - the existing index-entry pages following that state page match the same
    table, row id, index numbers, and key widths as the incoming replacement
    entries.
- Rewrite the row page in place at the same `row_id`.
- Rewrite only index-entry pages whose key bytes changed. Unchanged index pages
  keep their existing bytes.
- Rewrite buffered pages only. Already-flushed replacement runs keep the
  append-only path until page rewrites have undo or WAL coverage.
- Leave the existing row-state page in place. It still maps the original source
  row to the same current row id.
- Keep `header.page_count` unchanged and return the same row id as the updated
  row id.
- If any precondition fails, fall back to the existing append-only update path.

## Affected Subsystems

- MyLite durable storage update path.
- Active checkpoint append and rollback behavior.
- Exact-index, live-row-id, row-payload, and durable cache retargeting after
  updates.

## Compatibility Impact

SQL behavior does not change. The internal row id remains an implementation
detail. `ENGINE=InnoDB`, omitted/default, MyISAM, Aria, and explicit MyLite
routed tables use the same handler-visible update semantics.

## Single-File And Lifecycle Impact

No file-format, journal, WAL, lock, or companion-file change. The rewrite only
modifies pages created inside the current active checkpoint frame. Rollback
still truncates those pages; commit still publishes the existing header.

## Public API And File-Format Impact

No public API or file-format change.

## Binary-Size And Dependency Impact

First-party storage C change only. No new dependency.

## Tests And Verification

- Add storage coverage for repeated updates of the same replacement row inside
  an active transaction, verifying:
  - the second update returns the same row id;
  - unchanged index pages do not force a new row id;
  - changed secondary keys are visible and old secondary keys are hidden;
  - transaction rollback removes the rewritten row; and
  - explicit savepoint rollback does not rewrite rows that predate the
    savepoint frame.
- Run storage-smoke build targets, storage unit tests, embedded
  storage-engine tests, the full storage-smoke CTest suite, update performance
  baseline, `git diff --check`, and `git clang-format --diff`.

## Verification Results

- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
- `ctest --test-dir build/storage-smoke-dev --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 100000`
  measured direct primary-key updates at `15.979 us/op` and prepared
  primary-key updates at `9.905 us/op`.
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Acceptance Criteria

- Repeated updates of a row created in the current checkpoint can reuse the
  same row id and avoid appending another replacement chain.
- Savepoint rollback remains deterministic because rows created before the
  active savepoint frame are not rewritten.
- Exact-index and row-payload cache maintenance remains correct when
  `old_row_id == new_row_id`.
- Existing storage and embedded storage-engine tests remain green.

## Risks And Open Questions

- This is still not a pager or WAL. It reduces repeated-update write volume
  for rows already created inside the current checkpoint, but it does not
  provide page reuse, cross-process isolation, or a general B-tree update path.
- The first update from a durable row still appends a replacement chain. Full
  SQLite-like throughput will need maintained pages or a WAL-backed pager after
  this safe rewrite case is covered.
