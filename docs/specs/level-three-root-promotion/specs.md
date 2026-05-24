# Level Three Root Promotion

## Problem

MyLite can now maintain inserts through level-`2` branch roots, including full
lower-leaf splits and full lower-branch splits while the level-`2` root still
has child capacity. Once the level-`2` root page itself is child-cell-full, the
same insert shape falls back to append-tail index-entry pages even though the
existing read path can already navigate deeper branch roots.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB handler index mutation remains behind `handler` calls in
  `mariadb/sql/handler.h` and `mariadb/sql/handler.cc`; this slice stays in
  first-party MyLite storage.
- `packages/mylite-storage/src/storage.c` already contains:
  - `split_branch_index_root_entry()` for promoting a full level-`1` root to a
    level-`2` root;
  - `split_level_two_branch_child_entry()` for splitting a full lower
    level-`1` branch under a non-full level-`2` root;
  - recursive branch readers for exact, prefix, prefix-exists, and full
    entryset reads over multi-level branch roots.

## Scope

- For a level-`2` root whose selected lower level-`1` branch and selected leaf
  are both packed and full:
  - require the level-`2` root page itself to be child-cell-full;
  - read sibling lower-branch entry counts instead of assuming they are packed;
  - require no live append-tail row-state or index-entry overlay that would be
    hidden by appending new static pages;
  - split the selected leaf into the original leaf page plus one appended leaf;
  - split the selected lower level-`1` branch into the original lower branch
    page plus one appended lower branch page;
  - split the expanded level-`2` child list into two appended level-`2` branch
    pages;
  - rewrite the original root page as a level-`3` root over those two level-`2`
    branch pages.
- Protect the original root, selected lower branch, and selected leaf pages in
  the statement or transaction journal.
- Preserve fallback behavior for deeper full roots, live-overlay, unpacked
  branch, and unsupported key shapes.

## Non-Goals

- No arbitrary-depth recursive split implementation.
- No split of a full level-`3` root into a level-`4` root.
- No update/delete maintenance for level-`3` roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
extends static branch maintenance by one bounded level for supported fixed-width
raw index insertions.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. Existing dirty pages are
journal-protected. The inserted row page, split leaf page, appended lower
branch page, and two appended level-`2` branch pages are appended after the
statement-start header. Rollback and stale recovery restore protected pages and
truncate appended pages.

## File-Format Impact

No new page format or version. Existing branch pages already encode arbitrary
positive branch levels, and readers already validate and descend multi-level
branch roots.

## Test Plan

- Extend the packed-root split storage test after the level-`2` lower-branch
  split case until the level-`2` root page is child-cell-full and the selected
  lower branch is packed.
- Insert one more row that promotes the child-cell-full level-`2` root to a
  level-`3` root.
- Assert the promotion grows the file by one row page, one split leaf page, one
  split lower-branch page, and two level-`2` branch pages, not by a fallback
  index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full
  entryset reads, root level and child count, statement rollback, and final
  committed visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible child-cell-full level-`2` root inserts promote to a level-`3` root
  without an append-tail index-entry page.
- Deeper full-root and live-overlay cases keep existing fallback behavior.
- Rollback restores the previous level-`2` tree and file size.
- Existing level-`2` fitting, lower-leaf split, lower-branch split, read, and
  recovery tests remain passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- General arbitrary-depth split propagation remains future work.
- Level-`3` update/delete maintenance, balancing, and compaction remain future
  B-tree work.
