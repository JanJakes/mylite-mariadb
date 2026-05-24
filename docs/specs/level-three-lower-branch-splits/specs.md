# Level Three Lower Branch Splits

## Problem

Level-`3` branch roots now handle fitting inserts and full-leaf splits under a
selected lower level-`1` branch. Once that lower branch is child-cell-full,
inserts still fall back to append-tail index-entry pages even when the selected
level-`2` child branch has capacity for one more lower-branch child.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution reaches storage through `handler::ha_write_row()` in
  `mariadb/sql/handler.cc` and the virtual `handler::write_row()` contract in
  `mariadb/sql/handler.h`; this slice remains first-party MyLite storage work.
- `packages/mylite-storage/src/storage.c` already has the required local
  mechanics:
  - splitting a full leaf into the original page plus one appended leaf;
  - splitting an expanded level-`1` branch child list across two branch pages;
  - refreshing a parent branch from a rewritten child branch.
- The current level-`3` path already descends through root, level-`2` child,
  lower level-`1` branch, and selected leaf with fence validation.

## Scope

- For a level-`3` root whose selected path is:
  - a valid level-`2` child branch with room for one more lower-branch child;
  - a valid packed full level-`1` lower branch;
  - a selected full leaf;
  split the leaf and split the expanded lower-branch child list into the
  original lower branch plus one appended lower branch.
- Rewrite the selected level-`2` child branch with the extra lower-branch child
  cell.
- Refresh the level-`3` root branch from the rewritten level-`2` child branch.
- Require no live append-tail row-state or index-entry overlay that would be
  hidden by appending new static pages.
- Protect the root branch, level-`2` child branch, original lower branch, and
  original leaf pages in the statement or transaction journal.
- Preserve fallback behavior for level-`2` child full, level-`3` root split,
  live overlays, invalid fences, and broader split/merge cases.

## Non-Goals

- No split of a full level-`2` child branch under a level-`3` root.
- No split of a full level-`3` root into a level-`4` root.
- No arbitrary-depth recursive split propagation.
- No update/delete maintenance for level-`3` roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
only avoids another append-tail fallback for supported fixed-width raw indexes
after a level-`3` root has already been published.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. Existing dirty pages are
journal-protected. The inserted row page, split leaf page, and appended lower
branch page are appended after the statement-start header. Rollback restores
the previous branch pages and truncates appended pages.

## File-Format Impact

No file-format change. Existing branch and leaf page encodings are reused.

## Test Plan

- Extend the packed-root split storage test after the level-`3` lower-leaf
  split case until the selected lower level-`1` branch is packed and full while
  its level-`2` parent still has child capacity.
- Insert one more row that splits the selected lower branch.
- Assert the split grows the file by one row page, one split leaf page, and one
  appended lower-branch page, not by a fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full
  entryset reads, level-`2` child count, root entry count and level, statement
  rollback, and final committed visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible full-lower-branch inserts under level-`3` roots split the selected
  lower branch and refresh the level-`2` and level-`3` ancestors without an
  append-tail index-entry page.
- Full level-`2` child, live-overlay, and deeper-root cases keep existing
  fallback behavior.
- Rollback restores the previous level-`3` tree and file size.
- Existing level-`2`, level-`3` fitting, lower-leaf split, read, and recovery
  coverage remains passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Splitting a full level-`2` child branch under a level-`3` root remains future
  work.
- General arbitrary-depth insert, update, delete, balancing, and compaction
  remain future B-tree work.
