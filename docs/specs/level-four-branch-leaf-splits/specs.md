# Level Four Branch Leaf Splits

## Problem

Level-`4` roots can now maintain fitting inserts by rewriting the selected leaf
and four branch ancestors. Once the selected leaf is full, inserts below that
same level-`4` root still fall back to append-tail index-entry pages even when
the level-`1` lower branch has room for one more leaf child.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution reaches storage through `handler::ha_write_row()` in
  `mariadb/sql/handler.cc` and the `handler::write_row()` contract in
  `mariadb/sql/handler.h`; this slice remains first-party MyLite storage work.
- `packages/mylite-storage/src/storage.c` already has bounded lower-leaf splits
  below level-`2` and level-`3` roots, with live-overlay checks before appending
  static split leaves.
- Level-`4` fitting inserts already refresh level-`1`, level-`2`, level-`3`,
  and level-`4` branch fences after a leaf rewrite.

## Scope

- For a level-`4` root whose selected path is:
  - a valid level-`3` child branch;
  - a valid level-`2` grandchild branch;
  - a valid level-`1` lower branch;
  - a selected full leaf;
  - a lower branch with spare child capacity and packed current leaves;
  split the leaf into the existing leaf plus one appended leaf, rewrite the
  lower branch child list, and refresh all branch ancestors.
- Require no live append-tail row-state or index-entry overlay for that
  table/index after the current static branch subtree.
- Protect the existing root, level-`3` child, level-`2` child, level-`1` lower
  branch, and selected leaf pages in the statement or transaction journal.
- Preserve fallback behavior for full lower branches, live overlays, invalid
  fences, deeper roots, and higher split propagation.

## Non-Goals

- No split of a full level-`1` lower branch below a level-`4` root.
- No level-`2`, level-`3`, or level-`4` child-branch split propagation.
- No update/delete maintenance for level-`4` roots.
- No arbitrary-depth recursive insert maintenance.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
only removes one append-tail fallback for supported fixed-width raw indexes when
the selected full leaf can split under an existing level-`4` branch root.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. Existing dirty pages are
journal-protected. The inserted row page and one split leaf page are appended
after the statement-start header. Rollback restores the previous branch pages
and truncates appended pages.

## File-Format Impact

No file-format change. Existing branch page child cell and high-key fence fields
are updated.

## Test Plan

- Extend the packed-root split storage test after level-`4` fitting insert
  coverage by filling the selected level-`4` leaf.
- Add a statement-rolled-back full-leaf insert that appends one split leaf below
  the level-`4` root.
- Commit the same insert and assert the file grows by one row page plus one
  split leaf page, not by a fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full entryset
  reads, root entry count, root level, statement rollback, and final committed
  visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible full-leaf inserts under level-`4` roots split the selected leaf and
  refresh all branch ancestors without an append-tail index-entry page.
- Full lower-branch, higher split, live-overlay, and deeper-root cases keep
  existing fallback behavior.
- Rollback restores the previous level-`4` tree and file size.
- Existing level-`4` promotion and fitting insert coverage remains passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Level-`4` lower-branch and higher split propagation remains future work.
- General arbitrary-depth insert, update, delete, balancing, and compaction
  remain future B-tree work.
