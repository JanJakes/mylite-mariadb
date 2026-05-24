# Level Two Branch Leaf Splits

## Problem

Level-`2` branch roots now maintain inserts when the selected lower branch leaf
has spare capacity. Once that lower level-`1` leaf is full, inserts still fall
back to append-tail index-entry pages even when the lower branch has child-cell
capacity for a normal leaf split.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB handler index mutation remains behind `handler` calls in
  `mariadb/sql/handler.h` and `mariadb/sql/handler.cc`; this slice stays in
  first-party MyLite storage.
- `packages/mylite-storage/src/storage.c` already has single-level branch leaf
  splitting through `split_branch_index_leaf_entry()` and child-cell encoding
  through `copy_index_branch_children_with_split()`.
- The level-`2` insert path can already rewrite the lower branch page and then
  refresh the root branch fence.

## Scope

- For a level-`2` root whose selected lower child is a level-`1` branch:
  - detect a full selected leaf;
  - require lower-branch child capacity for one additional leaf;
  - require no live append-tail row-state or index-entry overlay that would be
    hidden by appending a new static leaf page;
  - rewrite the full leaf plus one appended split leaf;
  - rewrite the lower branch with the inserted child cell;
  - refresh the root branch child fence and entry count.
- Protect the root branch, lower branch, and original leaf pages in the
  statement or transaction journal.
- Preserve fallback behavior for lower-branch-full, deeper-root, live-overlay,
  and unsupported key shapes.

## Non-Goals

- No split of a full lower branch page.
- No root-level redistribution, merge, or arbitrary-depth B-tree split.
- No update/delete maintenance for level-`2` roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
only avoids an append-tail index-entry fallback for one more fixed-width raw
index insertion shape.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. The existing dirty pages are
journal-protected, and the new split leaf plus row page are appended after the
statement-start header. Rollback and stale recovery restore protected pages and
truncate appended pages.

## File-Format Impact

No new page format or version. Existing branch and leaf page encodings are
reused.

## Test Plan

- Extend the packed-root split storage test to fill the new level-`2` lower
  branch leaf through maintained inserts, then insert one more row that splits
  the full lower leaf.
- Assert the split grows the file by one row page plus one leaf page, not a
  fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full
  entryset reads, lower-branch child count, root entry count, statement
  rollback, and final committed visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible level-`2` full-leaf inserts split the lower branch leaf and refresh
  both branch levels without an append-tail index-entry page.
- Lower-branch-full and live-overlay cases keep existing fallback behavior.
- Rollback restores the previous level-`2` tree and file size.
- Existing branch root split, fitting insert, read, and recovery tests remain
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

- Lower-branch-full splits remain future work.
- Broader multi-level updates/deletes, balancing, and compaction remain future
  B-tree work.
