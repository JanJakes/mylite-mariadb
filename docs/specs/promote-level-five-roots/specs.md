# Promote Level Five Roots

## Problem

Deep branch roots can now split a child-cell-full level-`4` branch when the
selected level-`5` parent has child capacity. For the current reachable deepest
shape, that level-`5` parent is the root page. Once the level-`5` root is
child-cell-full, eligible inserts still fall back to append-tail index-entry
pages. Promoting a full level-`5` root to a bounded level-`6` root is the next
testable step before generic higher recursive propagation can be exercised.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB reaches this storage work through `handler::ha_write_row()` /
  `handler::write_row()` in `mariadb/sql/handler.cc` and
  `mariadb/sql/handler.h`; this remains first-party MyLite storage work.
- `packages/mylite-storage/src/storage.c` has fixed-depth root promotion
  through level `5` in `split_level_four_branch_upper_entry()`.
- Generic deep branch insert planning already captures the selected branch path
  and journal-protects the existing root, interior branch path, and full leaf.
- Sibling level-`4` entry counts must be read from referenced pages when
  splitting the expanded level-`5` root child list because prior level-`4`
  branch splits can leave sibling level-`4` branches non-uniform.

## Scope

- Extend the generic deep branch insert plan to cover full selected leaves when:
  - the root is exactly level `5`;
  - the selected leaf is full;
  - the selected level-`1` branch is packed and full;
  - the selected level-`2`, level-`3`, level-`4`, and root level-`5` branches
    are child-cell-full;
  - the static branch subtree has no live append-tail overlay for the
    table/index; and
  - the promoted level remains within the maintained branch level limit.
- Append one split leaf page, one new branch page at each level `1` through
  `4`, and two new level-`5` branch pages.
- Rewrite the original leaf, selected level-`1` through level-`4` branches, and
  the root page as a level-`6` root with two level-`5` children.

## Non-Goals

- No generic level-`5` child split under existing level-`6` parents.
- No root promotion above level `6`.
- No update/delete maintenance for deeper roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible ordering and lookup results remain unchanged. Eligible fixed-width
raw index inserts avoid an append-tail index-entry fallback when the current
level-`5` root can promote to a bounded level-`6` root.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. The inserted row page,
split leaf page, new level-`1` through level-`4` branch pages, and two new
level-`5` branch pages are appended after the statement-start header. Existing
root/path pages and the original full leaf are journal-protected. Rollback
restores the previous level-`5` root and truncates the appended pages.

## File-Format Impact

No file-format change. Existing leaf and branch page encodings are reused.

## Test Plan

- Extend the packed-root split storage test after the level-`5` level-four
  branch split by filling the level-`5` root and final level-`4`, level-`3`,
  level-`2`, level-`1`, and leaf path.
- Add a statement-rolled-back high-key insert that promotes the full level-`5`
  root to level `6`.
- Commit the same insert and assert the file grows by one row page, one split
  leaf page, one branch page at each level `1` through `4`, and two level-`5`
  branch pages.
- Verify root level, root child count, left/right level-`5` child counts,
  rollback, exact lookup, prefix lookup, indexed-row materialization, and full
  entryset reads.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible child-cell-full level-`5` roots promote to level `6` without an
  append-tail index-entry page.
- Root-full shapes above level `5` and broader recursive split-propagation
  cases keep existing fallback behavior.
- Statement rollback restores the previous level-`5` root and file size.
- Existing fitting-insert, leaf split, lower-branch split, child-branch split,
  upper-branch split, level-four branch split, lower-level split, promotion,
  read, and recovery coverage remains passing.

## Verification Results

- `clang-format -i packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Splitting level-`5` branches under existing level-`6` and deeper parents
  remains future recursive B-tree work.
- Deeper update/delete maintenance remains future work.
