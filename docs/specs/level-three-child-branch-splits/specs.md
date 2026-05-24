# Level Three Child Branch Splits

## Problem

Level-`3` roots can now split full lower level-`1` branches while the selected
level-`2` child branch still has capacity. Once that level-`2` child branch is
child-cell-full, inserts fall back to append-tail index-entry pages even when
the level-`3` root has capacity for one more level-`2` child branch.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution reaches storage through `handler::ha_write_row()` in
  `mariadb/sql/handler.cc` and the virtual `handler::write_row()` contract in
  `mariadb/sql/handler.h`; this slice remains first-party MyLite storage work.
- `packages/mylite-storage/src/storage.c` already has bounded split mechanics
  for:
  - splitting a full leaf;
  - splitting an expanded level-`1` branch into two lower branches;
  - splitting an expanded level-`2` child list into two level-`2` branches;
  - refreshing parent branch fences after child-branch rewrites.
- The current level-`3` path already validates root -> level-`2` child ->
  level-`1` lower branch fences before mutating the selected path.

## Scope

- For a level-`3` root whose selected path is:
  - a valid level-`2` child branch that is packed and full;
  - a valid packed full level-`1` lower branch;
  - a selected full leaf;
  - a level-`3` root with room for one more level-`2` child;
  split the leaf, split the lower branch, and split the expanded level-`2`
  child list into the original level-`2` child plus one appended level-`2`
  sibling.
- Rewrite the level-`3` root with the extra level-`2` child cell.
- Require no live append-tail row-state or index-entry overlay that would be
  hidden by appending new static pages.
- Protect the root branch, original level-`2` child branch, original lower
  branch, and original leaf pages in the statement or transaction journal.
- Preserve fallback behavior for level-`3` root full, live overlays, invalid
  fences, and broader recursive split cases.

## Non-Goals

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
journal-protected. The inserted row page, split leaf page, appended lower branch
page, and appended level-`2` child branch page are appended after the
statement-start header. Rollback restores the previous branch pages and
truncates appended pages.

## File-Format Impact

No file-format change. Existing branch and leaf page encodings are reused.

## Test Plan

- Extend the packed-root split storage test after the level-`3` lower-branch
  split case until the selected level-`2` child branch is packed and full while
  the level-`3` root still has child capacity.
- Insert one more row that splits the selected level-`2` child branch.
- Assert the split grows the file by one row page, one split leaf page, one
  appended lower-branch page, and one appended level-`2` branch page, not by a
  fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full
  entryset reads, root child count and level, statement rollback, and final
  committed visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible full level-`2` child inserts under level-`3` roots split the selected
  child branch and refresh the level-`3` root without an append-tail index-entry
  page.
- Full level-`3` root, live-overlay, and deeper-root cases keep existing
  fallback behavior.
- Rollback restores the previous level-`3` tree and file size.
- Existing level-`3` fitting, lower-leaf split, lower-branch split, read, and
  recovery coverage remains passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Promoting a full level-`3` root to level-`4` remains future work.
- General arbitrary-depth insert, update, delete, balancing, and compaction
  remain future B-tree work.
