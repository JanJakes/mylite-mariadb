# Promote Level-Ten Roots

## Problem Statement

MyLite can split a full non-root level-`9` branch when its level-`10` parent
has child capacity. If that level-`10` parent is the root and is also exactly
full, the insert still falls back to append-tail index-entry pages even though
a bounded promotion to a level-`11` root can preserve maintained branch pages.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB parser, optimizer, and handler behavior do not change. This slice is
  limited to first-party MyLite raw-index maintenance after the row write
  reaches the MyLite handler.
- `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES` is `16`; a level-`10`
  root insert protects the root-to-leaf branch path plus the selected leaf,
  staying within that bound.
- `MYLITE_STORAGE_INDEX_BRANCH_MAX_MAINTAINED_LEVEL` is
  `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES - 1`, so a level-`11`
  promoted root remains within the maintained branch level limit.
- Before this slice, `plan_deep_branch_index_root_insert()` already reached
  the selected level-`9` branch and level-`10` parent after the selected lower
  path is full.
  It split a full level-`9` branch only when the level-`10` parent had child
  capacity, and otherwise left a full level-`10` root on the append-tail
  fallback.
- `copy_index_branch_children_with_branch_split()` can expand a parent child
  list by replacing the selected child with left/right split branches and can
  read sibling entry counts when the expanded list itself must split.

## Scope

- Add a maintained-index insert plan flag for promoting an exactly full
  level-`10` root to a bounded level-`11` root.
- Reuse the selected-path, no-live-overlay prerequisites.
- Split the selected non-root level-`9` branch into the original left page plus
  one appended right level-`9` sibling.
- Split the expanded level-`10` root child list into two appended level-`10`
  branch pages and rewrite the original root page as level `11`.
- Add fixture-backed append, rollback, and commit coverage.

## Non-Goals

- No split of full non-root level-`10` branches under level-`11` or deeper
  parents.
- No level-`11` or deeper root promotion.
- No general recursive propagation.
- No public API, SQL compatibility, file-format version, storage-engine
  routing, or wire-protocol change.

## Design

When the selected leaf through selected level-`9` branch are full, and the
level-`10` root is exactly full, planning should set
`promote_level_ten_root`.

The writer keeps the lower split sequence unchanged through the selected
level-`8` branch. It expands and splits the selected level-`9` branch, using
the original selected level-`9` page as the left half and appending one right
level-`9` sibling. It then expands the level-`10` root child list by replacing
the selected level-`9` child with the two level-`9` halves, reads sibling
level-`9` entry counts, splits that expanded list into two appended level-`10`
pages, and rewrites the original root page as level `11` with those two
children.

## Compatibility Impact

No compatibility surface changes. This removes one internal append-tail
fallback for already supported fixed-width raw-index trees.

## DDL Metadata Routing Impact

No DDL metadata routing change. Existing index-root catalog records keep
pointing at the same root page.

## Single-File And Recovery Impact

All durable state remains in the primary `.mylite` file. Statement rollback and
stale-journal recovery must restore the original level-`10` root, selected
level-`9` branch, selected level-`8` branch, selected level-`7` branch,
selected level-`6` branch, lower selected path, and original file size.

## Public API, File Format, And Routing Impact

No public C API, file-format version, storage-engine routing, or wire-protocol
integration change. Existing branch-page encoding is reused.

## Build, Size, And Dependencies

No new dependencies or generated sources. The change adds one bounded plan flag
and extends the existing fixed-depth writer path.

## Test Plan

- Add fixture-backed storage coverage for a full level-`10` root with a full
  selected level-`9` child, full selected level-`8` child, full selected
  level-`7` child, and full selected level-`6` branch below it.
- Assert append promotes the root to level `11`, appends left/right level-`10`
  child pages, appends the right level-`9` sibling, rewrites selected
  level-`9`, level-`8`, level-`7`, and level-`6` branches as left halves, and
  preserves exact/prefix/indexed-row reads for the inserted row.
- Assert statement rollback restores root child count and level, branch pages,
  file size, and lookup absence.
- Run storage unit tests, static checks, and storage smoke.

## Acceptance Criteria

- Eligible full level-`10` roots promote to bounded level-`11` roots.
- Full non-root level-`10` parents remain explicit fallback behavior.
- Existing level-`9` root promotion and level-`9` branch split coverage remain
  passing.
- Rollback and commit preserve root metadata, branch fences, page count, and
  inserted-row lookup behavior.

## Risks And Open Questions

- This remains bounded fixed-depth propagation. Full non-root level-`10`
  branches and deeper roots should move toward a recursive propagation writer
  rather than continuing to grow one-off fixed-depth cases indefinitely.
