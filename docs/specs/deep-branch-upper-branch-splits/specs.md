# Deep Branch Upper Branch Splits

## Problem

Deep branch roots can now split a full selected level-`2` child branch when its
level-`3` parent still has child capacity. Once that level-`3` parent is
child-cell-full, eligible inserts still fall back to append-tail index-entry
pages even if the selected level-`4` parent has room for one more level-`3`
child. Level-specific writers already handle this through level `4`; deeper
roots need the same bounded propagation in the generic path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB reaches this storage work through `handler::ha_write_row()` /
  `handler::write_row()` in `mariadb/sql/handler.cc` and
  `mariadb/sql/handler.h`; this remains first-party MyLite storage work.
- `packages/mylite-storage/src/storage.c` already has the fixed-depth
  `split_level_four_branch_upper_entry()` writer for level-`4` roots.
- The generic deep branch insert path already records every branch page on the
  selected path and journal-protects that path plus the original leaf.
- Sibling level-`2` entry counts must be read from referenced pages while
  splitting a level-`3` branch because prior child-branch splits can leave
  sibling level-`2` branches non-uniform.

## Scope

- Extend the generic deep branch insert plan to cover full selected leaves when:
  - the root is deeper than level `4`;
  - the selected leaf is full;
  - the selected level-`1` branch is packed and full;
  - the selected level-`2` child branch is child-cell-full;
  - the selected level-`3` parent branch is child-cell-full;
  - the selected level-`4` parent branch has spare child capacity; and
  - the static branch subtree has no live append-tail overlay for the
    table/index.
- Append one split leaf page, one new level-`1` branch page, one new level-`2`
  branch page, and one new level-`3` branch page.
- Rewrite the original leaf, selected level-`1`, level-`2`, and level-`3`
  branches, the selected level-`4` parent branch, and higher ancestors on the
  selected path.

## Non-Goals

- No level-`4` or higher split propagation for deeper roots.
- No root promotion for level-`5` and deeper roots.
- No update/delete maintenance for deeper roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible ordering and lookup results remain unchanged. Eligible fixed-width
raw index inserts avoid an append-tail index-entry fallback when a deeper branch
tree can absorb one more level-`3` branch under the selected level-`4` parent.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. The inserted row page,
split leaf page, new level-`1` branch page, new level-`2` branch page, and new
level-`3` branch page are appended after the statement-start header. Existing
branch path pages and the original full leaf are journal-protected. Rollback
restores the previous deeper branch tree and truncates the appended pages.

## File-Format Impact

No file-format change. Existing leaf and branch page encodings are reused.

## Test Plan

- Extend the packed-root split storage test after the level-`5` child-branch
  split by filling the selected level-`3` parent and final level-`2`,
  level-`1`, and leaf path.
- Add a statement-rolled-back high-key insert that splits that full level-`3`
  parent under the level-`5` root.
- Commit the same insert and assert the file grows by one row page, one split
  leaf page, one level-`1` branch page, one level-`2` branch page, and one
  level-`3` branch page.
- Verify root level, selected level-`4` child count, left/right level-`3` child
  counts, rollback, exact lookup, prefix lookup, indexed-row materialization,
  and full entryset reads.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible child-cell-full level-`3` parent branches below level-`5` and deeper
  roots split under an existing level-`4` parent without an append-tail
  index-entry page.
- Full level-`4` parents and broader recursive split-propagation cases keep
  existing fallback behavior.
- Statement rollback restores the previous deeper branch tree and file size.
- Existing fitting-insert, leaf split, lower-branch split, child-branch split,
  lower-level split, promotion, read, and recovery coverage remains passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Recursive level-`4`, higher branch, and root split propagation remains future
  B-tree work.
- Deeper update/delete maintenance remains future work.
