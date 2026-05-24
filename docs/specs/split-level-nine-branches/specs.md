# Split Level-Nine Branches

## Problem Statement

MyLite can promote an exactly full level-`9` root to a bounded level-`10` root.
Before this slice, if the same full level-`9` branch was already below a
level-`10` parent with child capacity, the insert still fell back to
append-tail index-entry pages even though the parent could accept one appended
level-`9` sibling.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB parser, optimizer, and handler behavior do not change. This slice is
  limited to first-party MyLite raw-index maintenance after the row write
  reaches the MyLite handler.
- `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES` is `16`, and maintained
  deep-branch planning already supports selected paths up to
  `MYLITE_STORAGE_INDEX_BRANCH_MAX_MAINTAINED_LEVEL`.
- `plan_deep_branch_index_root_insert()` already detects the full selected
  level-`8` branch and its level-`9` parent. Before this slice, it promoted
  only when that level-`9` parent was the root, and otherwise left full non-root
  level-`9` parents on the fallback path.
- `copy_index_branch_children_with_branch_split()` can already expand a parent
  child list by replacing the selected child with left/right split branches.

## Scope

- Add a maintained-index insert plan flag for splitting a full non-root
  level-`9` branch when its level-`10` parent has child capacity.
- Reuse the selected-path, no-live-overlay prerequisites.
- Reuse the existing level-`8` and level-`9` split mechanics: rewrite the
  selected level-`9` branch as the left half, append the right level-`9`
  sibling, and update the level-`10` parent child list in place.
- Add fixture-backed append, rollback, and commit coverage.

## Non-Goals

- No split of full non-root level-`9` branches whose non-root level-`10`
  parent is also full.
- No level-`10` root promotion to level `11` in this slice; that follow-up is
  covered by `promote-level-ten-roots`.
- No general recursive propagation.
- No public API, SQL compatibility, file-format version, storage-engine
  routing, or wire-protocol change.

## Design

When the selected leaf through selected level-`8` branch are full, the selected
level-`9` branch is full, and its level-`10` parent has spare child capacity,
planning should set `split_level_nine_branch`.

The writer keeps the lower split sequence unchanged through the selected
level-`8` branch, then expands the selected level-`9` child list by replacing
the selected level-`8` child with the two level-`8` halves. It splits that
expanded list into the selected level-`9` page and one appended right level-`9`
sibling. It then expands the level-`10` parent child list with the two
level-`9` halves and rewrites the parent in place.

## Compatibility Impact

No compatibility surface changes. This removes one internal append-tail
fallback for already supported fixed-width raw-index trees.

## DDL Metadata Routing Impact

No DDL metadata routing change. Existing index-root catalog records keep
pointing at the same root page.

## Single-File And Recovery Impact

All durable state remains in the primary `.mylite` file. Statement rollback and
stale-journal recovery must restore the original level-`10` parent, selected
level-`9` branch, selected level-`8` branch, selected level-`7` branch,
selected level-`6` branch, lower selected path, and original file size.

## Public API, File Format, And Routing Impact

No public C API, file-format version, storage-engine routing, or wire-protocol
integration change. Existing branch-page encoding is reused.

## Build, Size, And Dependencies

No new dependencies or generated sources. The change adds one bounded plan flag
and extends the existing fixed-depth writer path.

## Test Plan

- Add fixture-backed storage coverage for a full level-`9` child below a
  level-`10` root with child capacity, with full selected level-`8`, level-`7`,
  and level-`6` branches below it.
- Assert append keeps the root at level `10`, increments its child count by
  one, appends the right level-`9` sibling, rewrites selected level-`9`,
  level-`8`, level-`7`, and level-`6` branches as left halves, and preserves
  exact/prefix/indexed-row reads for the inserted row.
- Assert statement rollback restores root child count, branch pages, file size,
  and lookup absence.
- Run storage unit tests, static checks, and storage smoke.

## Acceptance Criteria

- Eligible full non-root level-`9` branches split under level-`10` parents with
  child capacity.
- Full level-`9` branches whose non-root level-`10` parent is also full remain
  explicit fallback behavior.
- Existing level-`8` branch split and level-`9` root promotion coverage remain
  passing.
- Rollback and commit preserve root metadata, branch fences, page count, and
  inserted-row lookup behavior.

## Risks And Open Questions

- This remains bounded fixed-depth propagation. A full level-`10` root or
  deeper non-root propagation should move toward a recursive writer rather than
  continuing to grow one-off fixed-depth cases indefinitely.
