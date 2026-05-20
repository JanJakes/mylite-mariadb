# Cached Buffered Row Rewrite Fast Path

## Problem Statement

Prepared primary-key updates in one transaction now spend their largest
first-party storage frame in `rewrite_active_update_pages()`. The common
benchmark shape updates a non-indexed column through a stable primary-key
predicate, so the row id and index-entry page shape stay unchanged after the
first buffered rewrite for each row.

The existing shape cache already proves that the buffered row page and
replacement row-state page are suitable for in-place rewrite. Even when that
cache is hot and no index-entry pages changed, the rewrite path still rechecks
the state-page buffer reference and loops over index entries that cannot be
rewritten.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`), as recorded in
  `docs/architecture/engineering-standards.md`.
- MyLite first-party storage code owns the hot path in
  `packages/mylite-storage/src/storage.c`.
- `rewrite_active_update_pages()` already keeps a per-append-buffer
  `buffered_update_rewrites` shape cache keyed by row id.
- `buffered_update_rewrite_shape_known()` verifies table id and changed-index
  shape before the cached path is used.
- `buffered_append_page_range_contains_in_statement()` already proves that the
  row page, replacement row-state page, and any changed-index pages are resident
  in the active append buffer.
- A sampled one-million prepared-update run after deferred durable-cache
  retargeting showed `rewrite_active_update_pages()` as the largest storage
  frame. The same run also showed MariaDB `alloc_root()` and reset overhead, but
  those are outside this storage slice.

## Proposed Design

Add a narrow fast path inside `rewrite_active_update_pages()`:

- Keep the existing full validation for the first row rewrite and for any shape
  that is not cached.
- When the shape is cached and `changed_entry_count == 0`, skip replacement
  state-page lookup and both changed-index loops.
- Capture the current row page undo, rewrite the buffered row page, mark its
  checksum dirty, and report the rewrite.
- Leave changed-index updates, changed-index validation, rollback undo capture,
  and append-buffer range checks unchanged for all broader cases.

## Affected Subsystems

- First-party MyLite storage active append-buffer rewrite path.
- Statement rollback preimage capture for buffered row pages.
- Prepared-update storage performance baseline.

## Compatibility Impact

No SQL, C API, storage-engine routing, metadata, or file-format behavior
changes. The fast path is selected only after the existing row-shape cache has
validated the buffered row/state shape for the same row id and table id.

## Single-File And Embedded Lifecycle

No durable file or companion-file lifecycle change. The optimization changes
only process-local active append-buffer mutation before commit.

## Binary Size, License, And Dependencies

The change is a small first-party branch in existing C code. It adds no
dependencies and should have negligible binary-size impact.

## Test And Verification Plan

- Add or rely on existing storage coverage for repeated buffered updates,
  statement rollback, nested savepoint rollback, and unchanged-index update
  behavior.
- Build `mylite_storage_test`, `mylite_embedded_statement_test`,
  `mylite_embedded_storage_engine_test`, and `mylite_perf_baseline`.
- Run focused storage and embedded smoke tests.
- Run full `storage-smoke-dev` CTest.
- Run `git diff --check` and `git clang-format --diff`.
- Run a prepared-update performance baseline and sample the long run.

## Acceptance Criteria

- Cached-shape buffered row rewrites with zero changed index entries avoid
  state-page and index-page rewrite checks.
- First-use and changed-index buffered rewrites keep the existing validation.
- Statement rollback and nested savepoint rollback remain covered by passing
  storage and embedded tests.
- Prepared-update profiling shows lower `rewrite_active_update_pages()` cost or
  confirms that the next bottleneck moved elsewhere.

## Risks And Open Questions

- The shape cache lives on the append-buffer owner, while rollback undo lives
  on the current statement. The fast path must still capture row-page undo for
  each statement before rewriting.
- This does not address MariaDB `alloc_root()`/reset overhead or the remaining
  exact-index and row-payload cache lookup costs.
