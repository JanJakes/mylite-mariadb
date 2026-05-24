# Level Four Lower Branch Splits

## Problem

Level-`4` roots can split a full selected leaf when its level-`1` lower branch
still has child capacity. Once that lower branch is packed and full, inserts
below the same level-`4` root still fall back to append-tail index-entry pages
even when the selected level-`2` parent branch can accept one more lower-branch
child.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution reaches storage through `handler::ha_write_row()` in
  `mariadb/sql/handler.cc` and the `handler::write_row()` contract in
  `mariadb/sql/handler.h`; this slice remains first-party MyLite storage work.
- `packages/mylite-storage/src/storage.c` already has bounded lower-branch
  splits below level-`3` roots, including split-leaf preparation, lower-branch
  repartitioning, parent branch child-cell insertion, and parent fence refresh.
- Level-`4` leaf split support already protects and refreshes the selected
  level-`1`, level-`2`, level-`3`, and level-`4` branch path.

## Scope

- For a level-`4` root whose selected path is:
  - a valid level-`3` child branch;
  - a valid level-`2` grandchild branch with spare child capacity;
  - a valid packed full level-`1` lower branch;
  - a selected full leaf;
  split the leaf, repartition the expanded lower-branch child list into the
  existing lower branch plus one appended lower-branch sibling, insert the new
  lower-branch child into the level-`2` parent, and refresh level-`3` and
  level-`4` ancestors.
- Require no live append-tail row-state or index-entry overlay for that
  table/index after the current static branch subtree.
- Protect the existing root, level-`3` child, level-`2` child, level-`1` lower
  branch, and selected leaf pages in the statement or transaction journal.
- Preserve fallback behavior for full level-`2` parents, live overlays, invalid
  fences, deeper roots, and higher split propagation.

## Non-Goals

- No split of a full level-`2` child branch below a level-`4` root.
- No level-`3` or level-`4` child-branch split propagation.
- No update/delete maintenance for level-`4` roots.
- No arbitrary-depth recursive insert maintenance.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
only removes one append-tail fallback for supported fixed-width raw indexes when
a packed lower branch can split under an existing level-`4` branch root.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. Existing dirty pages are
journal-protected. The inserted row page, split leaf page, and one lower-branch
sibling page are appended after the statement-start header. Rollback restores
the previous branch pages and truncates appended pages.

## File-Format Impact

No file-format change. Existing branch page child cell, entry count, and
high-key fence fields are updated.

## Test Plan

- Extend the packed-root split storage test after level-`4` lower-leaf split
  coverage by filling the selected level-`1` branch below the level-`4` root.
- Add a statement-rolled-back full-leaf insert that splits the packed lower
  branch under the level-`4` root.
- Commit the same insert and assert the file grows by one row page, one split
  leaf page, and one lower-branch page, not by a fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full entryset
  reads, root entry count, root level, statement rollback, and final committed
  visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible packed lower-branch inserts under level-`4` roots split the selected
  lower branch and refresh all ancestors without an append-tail index-entry
  page.
- Full level-`2` parent, higher split, live-overlay, and deeper-root cases keep
  existing fallback behavior.
- Rollback restores the previous level-`4` tree and file size.
- Existing level-`4` promotion, fitting insert, and lower-leaf split coverage
  remains passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Level-`4` level-`2` child-branch and higher split propagation remains future
  work.
- General arbitrary-depth insert, update, delete, balancing, and compaction
  remain future B-tree work.
