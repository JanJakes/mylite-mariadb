# Level Four Upper Branch Splits

## Problem

Level-`4` roots can now split a packed level-`2` child branch when the selected
level-`3` parent still has room. Once that selected level-`3` child branch is
itself packed and full, inserts below the same level-`4` root still fall back to
append-tail index-entry pages even when the level-`4` root can accept one more
level-`3` child branch.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution reaches storage through `handler::ha_write_row()` in
  `mariadb/sql/handler.cc` and the `handler::write_row()` contract in
  `mariadb/sql/handler.h`; this slice remains first-party MyLite storage work.
- `packages/mylite-storage/src/storage.c` already has bounded level-`3` root
  child splits that can promote a full level-`3` root to level-`4` by splitting
  the expanded level-`3` child list into two level-`3` branch pages.
- Level-`4` child-branch split support already splits the selected leaf,
  level-`1` lower branch, and level-`2` child branch, then refreshes the
  selected level-`3` child branch and level-`4` root.

## Scope

- For a level-`4` root whose selected path is:
  - a valid level-`4` root with spare child capacity;
  - a valid packed full level-`3` child branch;
  - a valid packed full level-`2` child branch;
  - a valid packed full level-`1` lower branch;
  - a selected full leaf;
  split the leaf, lower branch, level-`2` child branch, and level-`3` child
  branch, insert the new level-`3` child branch into the level-`4` root, and
  avoid publishing an append-tail index-entry page.
- Require no live append-tail row-state or index-entry overlay for that
  table/index after the current static branch subtree.
- Protect the existing root, level-`3` child, level-`2` child, level-`1` lower
  branch, and selected leaf pages in the statement or transaction journal.
- Preserve fallback behavior for full level-`4` roots, live overlays, invalid
  fences, deeper roots, and promotion to level-`5`.

## Non-Goals

- No level-`4` root split or promotion to level-`5`.
- No arbitrary-depth recursive insert maintenance.
- No update/delete maintenance for level-`4` roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
only removes one append-tail fallback for supported fixed-width raw indexes when
a packed level-`3` child can split under an existing level-`4` branch root.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. Existing dirty pages are
journal-protected. The inserted row page, split leaf page, lower-branch sibling,
level-`2` child sibling, and level-`3` child sibling are appended after the
statement-start header. Rollback restores the previous branch pages and
truncates appended pages.

## File-Format Impact

No file-format change. Existing branch page child cell, entry count, and
high-key fence fields are updated.

## Test Plan

- Extend the packed-root split storage test after level-`4` child-branch split
  coverage by filling the selected level-`3` child branch below the level-`4`
  root.
- Add a statement-rolled-back full-leaf insert that splits the packed level-`3`
  child branch under the level-`4` root.
- Commit the same insert and assert the file grows by one row page, one split
  leaf page, one lower-branch page, one level-`2` child branch page, and one
  level-`3` child branch page, not by a fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full entryset
  reads, root entry count, root level, statement rollback, and final committed
  visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible packed level-`3` child inserts under level-`4` roots split the
  selected child branch and refresh the level-`4` root without an append-tail
  index-entry page.
- Full level-`4` root, live-overlay, invalid-fence, and deeper-root cases keep
  existing fallback behavior.
- Rollback restores the previous level-`4` tree and file size.
- Existing level-`4` promotion, fitting insert, lower-leaf split,
  lower-branch split, and child-branch split coverage remains passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Level-`4` root split or level-`5` promotion remains future work.
- General arbitrary-depth insert, update, delete, balancing, and compaction
  remain future B-tree work.
