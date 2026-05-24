# Level Four Root Promotion

## Problem

Level-`3` roots can now split fitting leaves, full lower branches, and full
level-`2` child branches while the root still has child capacity. Once the
level-`3` root page itself is child-cell-full, the same insert shape falls back
to append-tail index-entry pages even though recursive branch readers can
already descend deeper roots.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution reaches storage through `handler::ha_write_row()` in
  `mariadb/sql/handler.cc` and the virtual `handler::write_row()` contract in
  `mariadb/sql/handler.h`; this slice remains first-party MyLite storage work.
- `packages/mylite-storage/src/storage.c` already has bounded promotion from a
  full level-`2` root to a level-`3` root and now has bounded splitting of a
  full level-`2` child branch below a level-`3` root.
- Recursive branch readers already validate and descend positive branch levels
  above `3`; the file format does not need a new page type.

## Scope

- For a level-`3` root whose selected path is:
  - a valid packed full level-`2` child branch;
  - a valid packed full level-`1` lower branch;
  - a selected full leaf;
  - a child-cell-full level-`3` root;
  split the leaf, lower branch, and level-`2` child branch, then split the
  expanded level-`3` root child list into two appended level-`3` branch pages.
- Rewrite the original root page as a level-`4` root over those two level-`3`
  branch pages.
- Read sibling level-`2` branch entry counts instead of assuming all siblings
  are packed.
- Require no live append-tail row-state or index-entry overlay that would be
  hidden by appending new static pages.
- Protect the original root, original level-`2` child, original lower branch,
  and original leaf pages in the statement or transaction journal.
- Preserve fallback behavior for deeper full roots, live overlays, invalid
  fences, and arbitrary-depth recursive split cases.

## Non-Goals

- No split of a full level-`4` root into level-`5`.
- No arbitrary-depth recursive split propagation.
- No update/delete maintenance for level-`4` roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
extends static branch maintenance by one bounded level for supported fixed-width
raw index insertions.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. Existing dirty pages are
journal-protected. The inserted row page, split leaf page, appended lower branch
page, appended level-`2` child branch page, and two appended level-`3` branch
pages are appended after the statement-start header. Rollback restores the
previous branch pages and truncates appended pages.

## File-Format Impact

No file-format change. Existing branch pages already encode arbitrary positive
branch levels, and readers already descend multi-level branch roots.

## Test Plan

- Extend the packed-root split storage test after the level-`3` child-branch
  split case until the level-`3` root is child-cell-full and the selected path
  is packed.
- Insert one more row that promotes the level-`3` root to level-`4`.
- Assert the promotion grows the file by one row page, one split leaf page, one
  appended lower-branch page, one appended level-`2` branch page, and two
  appended level-`3` branch pages, not by a fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full
  entryset reads, root level and child count, statement rollback, and final
  committed visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible child-cell-full level-`3` root inserts promote to a level-`4` root
  without an append-tail index-entry page.
- Deeper full-root and live-overlay cases keep existing fallback behavior.
- Rollback restores the previous level-`3` tree and file size.
- Existing level-`3` fitting, lower-leaf, lower-branch, child-branch, read, and
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

- General arbitrary-depth insert, update, delete, balancing, and compaction
  remain future B-tree work.
