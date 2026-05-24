# Level Five Root Promotion

## Problem

Level-`4` roots can now maintain fitting inserts, full leaf splits, full
level-`1` lower-branch splits, full level-`2` child-branch splits, and full
level-`3` upper-branch splits while the root still has child capacity. Once the
level-`4` root page itself is child-cell-full, the same insert shape still falls
back to append-tail index-entry pages even though recursive branch readers can
already descend deeper roots.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution reaches storage through `handler::ha_write_row()` in
  `mariadb/sql/handler.cc` and the virtual `handler::write_row()` contract in
  `mariadb/sql/handler.h`; this slice remains first-party MyLite storage work.
- `packages/mylite-storage/src/storage.c` already has bounded root promotion
  from level-`2` to level-`3` and from level-`3` to level-`4`, and already has
  bounded level-`4` upper-branch splitting when the root has capacity.
- Recursive branch readers validate parent/child fences and descend positive
  branch levels above `4`, so the file format does not need a new page type.

## Scope

- For a level-`4` root whose selected path is:
  - a valid packed full level-`3` child branch;
  - a valid packed full level-`2` child branch;
  - a valid packed full level-`1` lower branch;
  - a selected full leaf;
  - a child-cell-full level-`4` root;
  split the leaf, lower branch, level-`2` child branch, and level-`3` child
  branch, then split the expanded level-`4` root child list into two appended
  level-`4` branch pages.
- Rewrite the original root page as a level-`5` root over those two level-`4`
  branch pages.
- Read sibling level-`3` branch entry counts instead of assuming all root
  children are packed.
- Require no live append-tail row-state or index-entry overlay that would be
  hidden by appending new static pages.
- Protect the original root, original level-`3` child, original level-`2`
  child, original level-`1` lower branch, and original leaf pages in the
  statement or transaction journal.
- Preserve fallback behavior for full level-`5` roots, live overlays, invalid
  fences, and arbitrary-depth recursive split cases.

## Non-Goals

- No insert maintenance below existing level-`5` roots.
- No arbitrary-depth recursive split propagation.
- No update/delete maintenance for level-`5` roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
extends static branch maintenance by one bounded promotion level for supported
fixed-width raw index insertions.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. Existing dirty pages are
journal-protected. The inserted row page, split leaf page, appended lower
branch page, appended level-`2` child branch page, appended level-`3` child
branch page, and two appended level-`4` branch pages are appended after the
statement-start header. Rollback restores the previous branch pages and
truncates appended pages.

## File-Format Impact

No file-format change. Existing branch pages already encode arbitrary positive
branch levels, and readers already descend multi-level branch roots.

## Test Plan

- Extend the packed-root split storage test after level-`4` upper-branch split
  coverage by filling the level-`4` root to child capacity and packing the
  selected path.
- Add a statement-rolled-back full-leaf insert that promotes the packed
  level-`4` root to level-`5`.
- Commit the same insert and assert the file grows by one row page, one split
  leaf page, one lower-branch page, one level-`2` child branch page, one
  level-`3` child branch page, and two level-`4` root child branch pages, not
  by a fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full
  entryset reads, root level and child count, statement rollback, and final
  committed visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible child-cell-full level-`4` root inserts promote to a level-`5` root
  without an append-tail index-entry page.
- Existing level-`4` fitting, lower-leaf, lower-branch, child-branch,
  upper-branch, read, and recovery coverage remains passing.
- Level-`5` follow-up insert maintenance remains explicit fallback behavior
  rather than an accidental claim of arbitrary-depth writes.
- Rollback restores the previous level-`4` tree and file size.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- General arbitrary-depth split propagation remains future B-tree work; this
  slice intentionally avoids adding level-`5` write maintenance.
- Level-`5` update/delete maintenance, balancing, and compaction remain future
  B-tree work.
