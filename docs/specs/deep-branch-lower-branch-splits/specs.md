# Deep Branch Lower Branch Splits

## Problem

Level-`5` and deeper branch roots can now maintain fitting inserts and split a
full selected leaf when the selected level-`1` branch still has child capacity.
When that level-`1` branch is packed and full, the same insert still falls back
to an append-tail index-entry page even if its level-`2` parent has room for one
more child branch. Existing level-specific writers already handle this shape up
through level `4`; deeper roots need the equivalent bounded split without
another fixed-depth writer.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB reaches this storage work through the same `handler::ha_write_row()` /
  `handler::write_row()` boundary in `mariadb/sql/handler.cc` and
  `mariadb/sql/handler.h`; this remains first-party MyLite storage code.
- `packages/mylite-storage/src/storage.c` has level-specific lower-branch split
  writers for level-`2`, level-`3`, and level-`4` roots.
- The reusable pieces are already present: `prepare_index_leaf_split_pages()`,
  `copy_index_branch_children_with_split()`,
  `refresh_index_branch_child_after_branch_insert()`, and the deep branch path
  reader used by fitting inserts and leaf splits.
- `index_branch_tail_has_live_overlay()` remains the guard against hiding live
  append-tail index-entry or row-state overlays when rewriting a static branch
  subtree.

## Scope

- Extend the generic deep branch insert plan to cover full selected leaves when:
  - the root is deeper than level `4`;
  - the selected leaf is full;
  - the selected level-`1` branch is packed and full;
  - the selected level-`2` parent branch has spare child capacity; and
  - the current static branch subtree has no live append-tail overlay for the
    table/index.
- Append one split leaf page and one new level-`1` branch page.
- Rewrite the original leaf, original level-`1` branch, selected level-`2`
  parent branch, and higher ancestors on the selected path.
- Protect the existing root-to-leaf branch path plus the original leaf in the
  statement or transaction journal.

## Non-Goals

- No level-`2` child-branch, higher branch, or root split propagation for deeper
  roots.
- No update/delete maintenance for deeper roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible ordering and lookup results remain unchanged. Eligible fixed-width
raw index inserts avoid an append-tail index-entry fallback when a deeper branch
tree can absorb one more level-`1` branch under the selected level-`2` parent.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. The inserted row page,
split leaf page, and new level-`1` branch page are appended after the
statement-start header. Existing branch path pages and the original full leaf
are journal-protected. Rollback restores the previous deeper branch tree and
truncates the appended pages.

## File-Format Impact

No file-format change. Existing leaf and branch page encodings are reused.

## Test Plan

- Extend the packed-root split storage test after the level-`5` leaf split by
  filling the selected level-`1` branch and its final leaf.
- Add a statement-rolled-back high-key insert that splits that full lower branch
  under the level-`5` root.
- Commit the same insert and assert the file grows by one row page, one split
  leaf page, and one level-`1` branch page, not by a fallback index-entry page.
- Verify root level, selected level-`2` child count, left/right lower branch
  child counts, rollback, exact lookup, prefix lookup, indexed-row
  materialization, and full entryset reads.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible packed full level-`1` branches below level-`5` and deeper roots split
  under an existing level-`2` parent without an append-tail index-entry page.
- Full level-`2` parents and broader split-propagation cases keep existing
  fallback behavior.
- Statement rollback restores the previous deeper branch tree and file size.
- Existing fitting-insert, deep leaf split, lower-level split, promotion, read,
  and recovery coverage remains passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Recursive level-`2`, higher branch, and root split propagation remains future
  B-tree work.
- Deeper update/delete maintenance remains future work.
