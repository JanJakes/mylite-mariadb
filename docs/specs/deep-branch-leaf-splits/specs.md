# Deep Branch Leaf Splits

## Problem

Fitting inserts below level-`5` and deeper branch roots now refresh the selected
branch path directly, but once the selected leaf becomes full the write path
still falls back to an append-tail index-entry page. Existing level-specific
writers can split a full leaf under level `1` through level `4` roots when the
lowest branch has child capacity. Deeper roots need the same first recursive
split step without adding another hard-coded level.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB reaches this work through the same `handler::ha_write_row()` /
  `handler::write_row()` boundary in `mariadb/sql/handler.cc` and
  `mariadb/sql/handler.h`; this slice remains first-party MyLite storage work.
- `packages/mylite-storage/src/storage.c` already has `split_branch_index_leaf_entry()`
  for level-`1` roots and level-specific split handling inside the level-`2`
  through level-`4` writers.
- `prepare_index_leaf_split_pages()` and `copy_index_branch_children_with_split()`
  already produce the split leaf pages and updated lower branch child cells.
- `index_branch_tail_has_live_overlay()` allows durable row pages after the
  static subtree and rejects live index-entry or row-state overlays that would
  be hidden by moving the static branch tail.

## Scope

- Extend the generic deep branch insert plan to cover full selected leaves when:
  - the root is deeper than level `4`;
  - the selected leaf is full;
  - the selected level-`1` branch has spare child capacity;
  - the level-`1` branch entry count matches its packed child-leaf capacity; and
  - the current static branch subtree has no live append-tail index-entry or
    row-state overlay for the table/index.
- Append one split leaf page and rewrite the existing selected leaf.
- Rewrite the selected level-`1` branch with one additional child cell and
  refresh higher branch fences and entry counts bottom-up.
- Protect the existing root-to-leaf branch path plus the original leaf in the
  statement or transaction journal.

## Non-Goals

- No lower-branch split when the level-`1` branch is full.
- No child-branch, upper-branch, or root split propagation for deeper roots.
- No update/delete maintenance for deeper roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible ordering and lookup results remain unchanged. Eligible fixed-width
raw index inserts avoid an append-tail index-entry page when a deeper branch
tree can absorb one more leaf under the selected level-`1` branch.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. The inserted row page
and split leaf page are appended after the statement-start header. Existing
branch path pages and the original full leaf are journal-protected. Rollback
restores the previous branch tree and truncates the appended row and split leaf
pages.

## File-Format Impact

No file-format change. Existing leaf and branch page encodings are reused.

## Test Plan

- Extend the packed-root split storage test after the level-`5` fitting insert
  coverage by inserting enough high keys to fill the newly split rightmost leaf.
- Add a statement-rolled-back high-key insert that splits that full leaf under
  the level-`5` root.
- Commit the same insert and assert the file grows by one row page and one
  split leaf page, not by a fallback index-entry page.
- Verify root level, child counts, rollback, exact lookup, prefix lookup,
  indexed-row materialization, and full entryset reads.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible full selected leaves below level-`5` and deeper roots split under an
  existing level-`1` branch without an append-tail index-entry page.
- Full level-`1` branches and broader split-propagation cases keep existing
  fallback behavior.
- Statement rollback restores the previous deeper branch tree and file size.
- Existing fitting-insert, lower-level split, promotion, read, and recovery
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

- Recursive lower-branch and higher branch split propagation remains future
  B-tree work.
- Deeper update/delete maintenance remains future work.
