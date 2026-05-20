# Row Update Row-Only Rewrite Fast Path

## Problem

Repeated active updates can rewrite buffered unpublished row pages in place.
When an update does not change any indexed key image, the current helper still
enters the generic active-update rewrite path. After the row/state page shape is
already validated, that path checks the generic changed-index shape cache and
sets up changed-index rewrite bookkeeping even though there are no changed
index pages to process.

The prepared primary-key update benchmark mostly exercises this row-only case:
`UPDATE perf_rows SET value = value + 1 WHERE id = ?` changes row payload bytes
while the primary-key image is stable.

## Source Findings

- Base source authority: MariaDB 11.8.6
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB file changes.
- `rewrite_active_update_pages()` computes `changed_entry_count` from the
  handler-provided changed-index vector.
- When `changed_entry_count == 0`, a cached shape lets the helper capture the
  row-page undo, rewrite the row payload, mark the buffered page checksum dirty,
  and return without touching index-entry pages.
- `buffered_update_rewrite_shape_known()` remains generic: it walks
  `index_entry_count` to prove the changed-index vector matches the cached
  shape.

## Design

- Add a row-only shape lookup that checks only the row id, table id, and cached
  zero changed-index count.
- Add a row-only rewrite helper used when `changed_entry_count == 0`.
- Preserve first-use validation: row page metadata and the paired row-state page
  are still decoded before the helper marks a reusable shape.
- Keep the existing generic helper for updates that change one or more indexed
  key images.
- Keep rollback semantics unchanged: the statement-level row-page undo is still
  captured before any buffered row bytes are modified.

## Compatibility Impact

No SQL, public API, storage-engine routing, metadata, or file-format behavior
changes. The optimization only narrows internal control flow for active
unpublished row rewrites with stable index keys.

## Single-File And Lifecycle Impact

No durable lifecycle changes. Buffered row pages, row-state pages, savepoint
undo, journal publication, and header publication keep the same ownership.

## Binary-Size Impact

Negligible. The change adds one small helper path and no dependencies or public
symbols.

## Tests And Verification

- Run:
  - `git diff --check`
  - `git clang-format --diff`
  - `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_statement_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  - `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_statement_test`
  - `build/storage-smoke-dev/packages/libmylite/mylite_embedded_storage_engine_test`
  - `ctest --preset storage-smoke-dev --output-on-failure`
  - `tools/mylite-perf-baseline --phase=prepared-updates 1000 1000000`

## Acceptance Criteria

- Row-only active updates use the dedicated helper after
  `changed_entry_count == 0`.
- First-use validation still rejects wrong table ids, overflow row pages,
  missing row-state pages, wrong replacement ids, and non-replace row states.
- Statement/savepoint rollback tests still cover buffered row rewrites.
- The prepared-update benchmark completes with the expected checksum.

## Risks And Unresolved Questions

- The row-only helper must not trust a cached shape unless the cache records a
  zero changed-index count for the same row and table.
- The largest remaining benchmark cost is MariaDB's per-execute plan work; this
  only trims first-party storage rewrite overhead.
