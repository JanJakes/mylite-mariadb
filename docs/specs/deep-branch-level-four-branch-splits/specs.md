# Deep Branch Level Four Branch Splits

## Problem

Deep branch roots can now split a full selected level-`3` branch when its
level-`4` parent still has child capacity. Once that level-`4` parent is
child-cell-full, eligible inserts still fall back to append-tail index-entry
pages even if the selected level-`5` parent has room for one more level-`4`
child. The fixed level-`4` root writer already knows how to split through a
level-`3` branch; the generic deep path needs one more bounded propagation step.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB reaches this storage work through `handler::ha_write_row()` /
  `handler::write_row()` in `mariadb/sql/handler.cc` and
  `mariadb/sql/handler.h`; this remains first-party MyLite storage work.
- `packages/mylite-storage/src/storage.c` now has generic deep split writers
  through selected level-`3` branches and fixed-depth reference writers through
  level-`4` root promotion.
- The deep branch insert plan already records every branch page on the selected
  path, which is enough to journal-protect the existing pages rewritten by this
  slice.
- Sibling level-`3` entry counts must be read from referenced pages while
  splitting a level-`4` branch because prior upper-branch splits can leave
  sibling level-`3` branches non-uniform.

## Scope

- Extend the generic deep branch insert plan to cover full selected leaves when:
  - the root is deeper than level `4`;
  - the selected leaf is full;
  - the selected level-`1` branch is packed and full;
  - the selected level-`2` branch is child-cell-full;
  - the selected level-`3` branch is child-cell-full;
  - the selected level-`4` branch is child-cell-full;
  - the selected level-`5` parent branch has spare child capacity; and
  - the static branch subtree has no live append-tail overlay for the
    table/index.
- Append one split leaf page and one new branch page at each level `1` through
  `4`.
- Rewrite the original leaf, selected level-`1` through level-`4` branches, the
  selected level-`5` parent branch, and higher ancestors on the selected path.

## Non-Goals

- No level-`5` or higher split propagation.
- No root promotion for level-`5` and deeper roots.
- No update/delete maintenance for deeper roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible ordering and lookup results remain unchanged. Eligible fixed-width
raw index inserts avoid an append-tail index-entry fallback when a deeper branch
tree can absorb one more level-`4` branch under the selected level-`5` parent.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. The inserted row page,
split leaf page, and new level-`1` through level-`4` branch pages are appended
after the statement-start header. Existing branch path pages and the original
full leaf are journal-protected. Rollback restores the previous deeper branch
tree and truncates the appended pages.

## File-Format Impact

No file-format change. Existing leaf and branch page encodings are reused.

## Test Plan

- Extend the packed-root split storage test after the level-`5` upper-branch
  split by filling the selected level-`4` branch and final level-`3`,
  level-`2`, level-`1`, and leaf path.
- Add a statement-rolled-back high-key insert that splits that full level-`4`
  branch under the level-`5` root.
- Commit the same insert and assert the file grows by one row page, one split
  leaf page, and one new branch page at each level `1` through `4`.
- Verify root level, selected level-`5` child count, left/right level-`4` child
  counts, rollback, exact lookup, prefix lookup, indexed-row materialization,
  and full entryset reads.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible child-cell-full level-`4` branches below level-`5` and deeper roots
  split under an existing level-`5` parent without an append-tail index-entry
  page.
- Full level-`5` parents and broader recursive split-propagation cases keep
  existing fallback behavior.
- Statement rollback restores the previous deeper branch tree and file size.
- Existing fitting-insert, leaf split, lower-branch split, child-branch split,
  upper-branch split, lower-level split, promotion, read, and recovery coverage
  remains passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Recursive level-`5`, higher branch, and root split propagation remains future
  B-tree work.
- Deeper update/delete maintenance remains future work.
