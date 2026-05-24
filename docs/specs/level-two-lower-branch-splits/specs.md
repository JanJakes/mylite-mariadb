# Level Two Lower Branch Splits

## Problem

Level-`2` branch roots can now maintain inserts into non-full lower leaves and
split full lower leaves when the selected lower level-`1` branch has child
capacity. Once that lower branch page itself is full, inserts still fall back
to append-tail index-entry pages even when the level-`2` root has room for one
more lower branch child.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB index mutation remains behind handler calls in
  `mariadb/sql/handler.h` and `mariadb/sql/handler.cc`; this slice stays in
  first-party MyLite storage.
- `packages/mylite-storage/src/storage.c` already has single-level branch-root
  promotion in `split_branch_index_root_entry()`, including:
  - splitting a full leaf into two leaf pages;
  - expanding the child-cell list with
    `copy_index_branch_children_with_split()`;
  - splitting the expanded child-cell list across two level-`1` branch pages.
- The level-`2` insert path already reads the root, selected lower branch, and
  selected leaf, and it can refresh the root branch cell from a rewritten lower
  branch.

## Scope

- For a level-`2` root whose selected lower child is a full level-`1` branch:
  - require the selected leaf to be full and the lower branch to be packed;
  - require the level-`2` root to have child capacity for one additional lower
    branch;
  - require no live append-tail row-state or index-entry overlay that would be
    hidden by appending new static pages;
  - split the selected leaf into the original leaf page plus one appended leaf;
  - split the expanded lower-branch child list into the original lower branch
    page plus one appended lower branch page;
  - rewrite the root branch page with both lower branch child cells and the
    updated root entry count.
- Protect the root branch, original lower branch, and original leaf pages in
  the statement or transaction journal.
- Preserve fallback behavior for root-full, unpacked lower-branch,
  live-overlay, deeper-root, and unsupported key shapes.

## Non-Goals

- No split of a full level-`2` root into a level-`3` root.
- No arbitrary-depth B-tree split, merge, borrow, or redistribution.
- No update/delete maintenance for level-`2` roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
only avoids another append-tail index-entry fallback for supported fixed-width
raw indexes after a level-`2` root has already been published.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. Existing dirty pages are
journal-protected. The inserted row page, split leaf page, and new lower branch
page are appended after the statement-start header. Rollback and stale recovery
restore protected pages and truncate appended pages.

## File-Format Impact

No new page format or version. Existing branch and leaf encodings are reused.

## Test Plan

- Extend the packed-root split storage test past the level-`2` lower-leaf split
  case until the selected lower branch page is full.
- Insert one more row that splits the full lower branch while the root has child
  capacity.
- Assert the split grows the file by one row page, one split leaf page, and one
  split lower-branch page, not by a fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full
  entryset reads, lower-branch child counts, root child count, root entry count,
  statement rollback, and final committed visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible level-`2` full-lower-branch inserts split the selected lower branch
  and refresh the root without an append-tail index-entry page.
- Root-full and live-overlay cases keep existing fallback behavior.
- Rollback restores the previous level-`2` tree and file size.
- Existing fitting insert, lower-leaf split, read, and recovery tests remain
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

- Splitting a full level-`2` root into a deeper tree remains future work.
- Broader update/delete maintenance, balancing, and compaction remain future
  B-tree work.
