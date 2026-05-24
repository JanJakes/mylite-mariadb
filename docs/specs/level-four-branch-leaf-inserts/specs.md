# Level Four Branch Leaf Inserts

## Problem

After a child-cell-full level-`3` root promotes to a level-`4` root, readers can
descend the deeper static tree. The next insert into a non-full leaf under that
level-`4` tree still falls back to append-tail index-entry pages because insert
maintenance only rewrites through level-`3` roots.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB index mutation remains behind `handler::ha_write_row()` in
  `mariadb/sql/handler.cc` and the `handler::write_row()` contract in
  `mariadb/sql/handler.h`; this slice stays in first-party MyLite storage.
- `packages/mylite-storage/src/storage.c` already maintains fitting inserts
  through level-`3` roots by rewriting the selected leaf, level-`1` lower
  branch, level-`2` child branch, and level-`3` root.
- The existing branch refresh helpers can update a parent branch from either a
  rewritten leaf child or a rewritten branch child, so the level-`4` fitting
  shape needs no new page format.

## Scope

- For a level-`4` root whose selected path is:
  - a valid level-`3` child branch;
  - a valid level-`2` grandchild branch;
  - a valid level-`1` lower branch;
  - a selected leaf with spare capacity;
  rewrite the leaf, level-`1` lower branch, level-`2` child branch, level-`3`
  child branch, and level-`4` root directly.
- Update entry counts and high-key fences at all four branch levels.
- Protect all five dirty existing pages in the statement or transaction
  journal.
- Preserve append-tail fallback for full leaves, deeper roots, invalid fences,
  live-overlay split shapes, and arbitrary split/merge cases.

## Non-Goals

- No level-`4` leaf split.
- No lower-branch, level-`2`, level-`3`, or root split under a level-`4` root.
- No update/delete maintenance for level-`4` roots.
- No arbitrary-depth recursive insert maintenance.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
only removes one append-tail fallback for supported fixed-width raw indexes
after a level-`4` root has already been published.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. The leaf, level-`1`, level-`2`,
level-`3`, and root branch pages are journal-protected before mutation. The
inserted row page is appended and becomes durable when the header page count is
published. Rollback restores the previous branch pages and truncates the
appended row page.

## File-Format Impact

No file-format change. Existing branch page level, child page id, entry count,
and high-key fence fields are updated.

## Test Plan

- Extend the packed-root split storage test after level-`4` promotion with a
  statement-rolled-back fitting insert into the promoted level-`4` tree.
- Commit the same insert and assert the file grows by one row page only, not by
  a fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full entryset
  reads, root entry count, root level, statement rollback, and final committed
  visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible fitting inserts under level-`4` roots rewrite all branch ancestors
  without an append-tail index-entry page.
- Full-leaf, split-required, and deeper-root cases keep existing fallback
  behavior.
- Rollback restores the previous level-`4` tree and file size.
- Existing level-`2`, level-`3`, and level-`4` promotion coverage remains
  passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Level-`4` split propagation remains future work.
- General arbitrary-depth insert, update, delete, balancing, and compaction
  remain future B-tree work.
