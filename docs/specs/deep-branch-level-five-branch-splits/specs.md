# Deep Branch Level Five Branch Splits

## Problem

Level-`5` roots can now promote to bounded level-`6` roots. After that
promotion, inserts below the rightmost level-`5` child can still split leaves,
level-`1`, level-`2`, level-`3`, and level-`4` branches while the selected
level-`5` child has capacity. Once that level-`5` child is child-cell-full, the
same no-overlay insert shape falls back to append-tail index-entry pages even
though the level-`6` parent root can usually accept one more level-`5` child.
The next bounded recursive split step is to split that full level-`5` branch
under the existing level-`6` parent.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB reaches this storage work through `handler::ha_write_row()` calling
  `write_row()` in `mariadb/sql/handler.cc`, with the non-virtual handler API
  declared in `mariadb/sql/handler.h`. This slice remains first-party MyLite
  storage work below that handler boundary.
- `packages/mylite-storage/src/storage.c` already records the selected branch
  path for deep maintained inserts and can journal-protect every existing branch
  page rewritten by this bounded split.
- `split_deep_branch_level_four_entry()` already prepares the split leaf,
  split level-`1` through level-`4` branch pages, expanded level-`5` child list,
  and sibling level-`4` entry counts needed for root promotion. The same lower
  split preparation can be reused when the full level-`5` branch has a parent
  with child capacity.

## Scope

- Extend the generic deep branch insert plan to cover full selected leaves when:
  - the root is at least level `6`;
  - the selected leaf is full;
  - the selected level-`1` branch is packed and full;
  - the selected level-`2`, level-`3`, level-`4`, and level-`5` branches are
    child-cell-full;
  - the selected level-`6` parent branch has spare child capacity;
  - the static branch subtree has no live append-tail overlay for the
    table/index; and
  - the dirty path fits in the bounded maintained-index journal plan.
- Append one split leaf page, one new branch page at each level `1` through
  `4`, and one new right level-`5` branch page.
- Rewrite the original leaf, selected level-`1` through level-`5` branches, the
  selected level-`6` parent, and any higher ancestors on the selected path.

## Non-Goals

- No split of child-cell-full level-`6` parents.
- No root promotion above level `6`.
- No update/delete maintenance for deeper roots.
- No file-format, SQL, public API, storage-engine routing, wire-protocol,
  binary-size, license, or dependency change.

## Compatibility Impact

SQL-visible ordering and lookup results remain unchanged. Eligible fixed-width
raw index inserts avoid an append-tail index-entry fallback when an existing
level-`6` branch tree can absorb one more level-`5` child.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. The inserted row page,
split leaf page, new level-`1` through level-`4` branch pages, and one new
level-`5` branch page are appended after the statement-start header. Existing
branch path pages and the original full leaf are journal-protected. Rollback
restores the previous level-`6` tree and truncates the appended pages.

## File-Format Impact

No file-format change. Existing leaf and branch page encodings are reused.

## Test Plan

- Extend the packed-root split storage test after level-`6` root promotion by
  filling the rightmost level-`5`, level-`4`, level-`3`, level-`2`, level-`1`,
  and leaf path.
- Add a statement-rolled-back high-key insert that splits the full selected
  level-`5` branch under the level-`6` root.
- Commit the same insert and assert the file grows by one row page, one split
  leaf page, one new branch page at each level `1` through `4`, and one new
  level-`5` branch page.
- Verify root level, root child count, left/right level-`5` child counts,
  rollback, exact lookup, prefix lookup, indexed-row materialization, and full
  entryset reads.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible child-cell-full level-`5` branches under level-`6` and deeper roots
  split under an existing parent without an append-tail index-entry page.
- Full level-`6` parents and broader recursive split-propagation cases keep
  existing fallback behavior.
- Statement rollback restores the previous level-`6` tree and file size.
- Existing fitting-insert, leaf split, lower-branch split, child-branch split,
  upper-branch split, level-four branch split, level-`6` root promotion, read,
  and recovery coverage remains passing.

## Verification Results

- `clang-format -i packages/mylite-storage/src/storage.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `clang-format -i packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Splitting full level-`6` parents and deeper recursive propagation remains
  future B-tree work.
- Deeper update/delete maintenance remains future work.
