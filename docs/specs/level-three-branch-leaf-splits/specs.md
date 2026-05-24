# Level Three Branch Leaf Splits

## Problem

Fitting inserts below promoted level-`3` roots now rewrite the selected leaf,
lower level-`1` branch, level-`2` child branch, and level-`3` root. Once that
selected leaf is full, inserts still fall back to append-tail index-entry pages
even when the lower level-`1` branch has child-cell capacity for a normal leaf
split.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution calls storage handlers through
  `handler::ha_write_row()` in `mariadb/sql/handler.cc` and the virtual
  `handler::write_row()` contract in `mariadb/sql/handler.h`; this slice stays
  in first-party MyLite storage.
- `packages/mylite-storage/src/storage.c` already has single-level leaf split
  helpers through `prepare_index_leaf_split_pages()` and
  `copy_index_branch_children_with_split()`.
- The level-`2` insert path can already split a full lower leaf, rewrite the
  lower branch with an extra child cell, and refresh its parent branch fence.
- The level-`3` fitting insert path can already descend root -> level-`2`
  child -> level-`1` lower branch -> leaf and refresh all three branch
  ancestors after a leaf rewrite.

## Scope

- For a level-`3` root whose selected path is:
  - a valid level-`2` child branch;
  - a valid level-`1` lower branch;
  - a selected full leaf;
  - a lower branch with room for one additional leaf child;
  split the leaf into the original page plus one appended leaf page.
- Rewrite the lower level-`1` branch with the extra child cell.
- Refresh the level-`2` child branch and level-`3` root branch from the
  rewritten lower branch.
- Require no live append-tail row-state or index-entry overlay that would be
  hidden by appending a new static leaf page.
- Protect the root branch, level-`2` child branch, lower branch, and original
  leaf pages in the statement or transaction journal.
- Preserve fallback behavior for full lower branches, live overlays, deeper
  roots, invalid fences, and broader split/merge cases.

## Non-Goals

- No split of a full lower level-`1` branch under a level-`3` root.
- No split of a full level-`2` child branch or level-`3` root.
- No update/delete maintenance for level-`3` roots.
- No arbitrary-depth recursive insert maintenance.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
only avoids another append-tail fallback for supported fixed-width raw indexes
after a level-`3` root has already been published.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. Existing dirty pages are
journal-protected. The inserted row page and split leaf page are appended after
the statement-start header. Rollback restores the previous branch pages and
truncates appended pages.

## File-Format Impact

No file-format change. Existing branch and leaf page encodings are reused.

## Test Plan

- Extend the packed-root split storage test after level-`3` fitting inserts
  until the selected leaf below the promoted level-`3` root is full while its
  lower level-`1` branch still has child capacity.
- Insert one more row that splits that full leaf.
- Assert the split grows the file by one row page plus one split leaf page,
  not by a fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full
  entryset reads, lower-branch child count, root entry count and level,
  statement rollback, and final committed visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible full-leaf inserts under level-`3` roots split the selected leaf and
  refresh all three branch levels without an append-tail index-entry page.
- Full-lower-branch, live-overlay, and deeper-root cases keep existing fallback
  behavior.
- Rollback restores the previous level-`3` tree and file size.
- Existing level-`2`, level-`3` promotion, fitting insert, read, and recovery
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

- Full lower-branch splits under level-`3` roots remain future work.
- General arbitrary-depth insert, update, delete, balancing, and compaction
  remain future B-tree work.
