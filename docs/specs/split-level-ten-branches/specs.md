# Split Level-Ten Branches

## Problem Statement

MyLite can promote an exactly full level-`10` root to a bounded level-`11`
root. After that promotion, a later insert can target a full non-root
level-`10` branch below a level-`11` parent that still has child capacity. That
shape currently falls back to append-tail index-entry pages even though the
level-`11` parent can accept one appended level-`10` sibling.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB parser, optimizer, and handler behavior do not change. This slice is
  limited to first-party MyLite raw-index maintenance after the row write
  reaches the MyLite handler.
- `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES` is `16`; a level-`11`
  branch-root insert protects the root-to-leaf branch path plus the selected
  leaf, staying within that bound.
- `MYLITE_STORAGE_INDEX_BRANCH_MAX_MAINTAINED_LEVEL` is
  `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES - 1`, so maintained
  level-`11` roots are within the existing bounded writer envelope.
- `plan_deep_branch_index_root_insert()` already descends through the selected
  level-`10` branch for level-`11` roots. Before this slice, when the selected
  level-`10` branch was full, planning only handled the level-`10` root case
  and otherwise left the non-root branch on the append-tail fallback.
- `copy_index_branch_children_with_branch_split()` can expand a parent child
  list by replacing the selected child with left/right split branches. It can
  also read sibling entry counts for the full child list that must itself be
  split.

## Scope

- Add a maintained-index insert plan flag for splitting a full non-root
  level-`10` branch when its level-`11` parent has child capacity.
- Reuse the selected-path, no-live-overlay prerequisites.
- Reuse the existing lower split sequence through the selected level-`9`
  branch.
- Split the selected level-`10` branch into the original left page plus one
  appended right level-`10` sibling.
- Rewrite the level-`11` parent in place with the two level-`10` halves.
- Add fixture-backed append, rollback, and commit coverage.

## Non-Goals

- No split of full non-root level-`10` branches whose level-`11` parent is also
  full.
- No level-`11` or deeper root promotion.
- No general recursive propagation.
- No public API, SQL compatibility, file-format version, storage-engine
  routing, or wire-protocol change.

## Design

When the selected leaf through selected level-`10` branch are full, and the
level-`11` parent has spare child capacity, planning should set
`split_level_ten_branch`.

The writer keeps the lower split sequence unchanged through the selected
level-`9` branch. It expands and splits the selected level-`10` branch, using
the original selected level-`10` page as the left half and appending one right
level-`10` sibling. It then expands the level-`11` parent child list by
replacing the selected level-`10` child with the two level-`10` halves and
rewrites the parent in place.

## Compatibility Impact

No compatibility surface changes. This removes one internal append-tail
fallback for already supported fixed-width raw-index trees.

## DDL Metadata Routing Impact

No DDL metadata routing change. Existing index-root catalog records keep
pointing at the same root page.

## Single-File And Recovery Impact

All durable state remains in the primary `.mylite` file. Statement rollback and
stale-journal recovery must restore the original level-`11` parent, selected
level-`10` branch, selected level-`9` branch, selected level-`8` branch,
selected level-`7` branch, selected level-`6` branch, lower selected path, and
original file size.

## Public API, File Format, And Routing Impact

No public C API, file-format version, storage-engine routing, or wire-protocol
integration change. Existing branch-page encoding is reused.

## Build, Size, And Dependencies

No new dependencies or generated sources. The change adds one bounded plan flag
and extends the existing fixed-depth writer path.

## Test Plan

- Add fixture-backed storage coverage for a level-`11` root with child capacity
  and a full selected level-`10` child, full selected level-`9` child, full
  selected level-`8` child, full selected level-`7` child, and full selected
  level-`6` branch below it.
- Assert append keeps the root at level `11`, increments its child count,
  appends the right level-`10` sibling, rewrites selected level-`10`,
  level-`9`, level-`8`, level-`7`, and level-`6` branches as left halves, and
  preserves exact/prefix/indexed-row reads for the inserted row.
- Assert statement rollback restores root child count, branch pages, file size,
  and lookup absence.
- Run storage unit tests, static checks, and storage smoke.

## Acceptance Criteria

- Eligible full non-root level-`10` branches split under level-`11` parents
  with child capacity.
- Full level-`10` branches whose level-`11` parent is also full remain explicit
  fallback behavior.
- Existing level-`10` root promotion, level-`9` branch split, and level-`9`
  root promotion coverage remain passing.
- Rollback and commit preserve root metadata, branch fences, page count, and
  inserted-row lookup behavior.

## Risks And Open Questions

- This keeps the current bounded fixed-depth propagation model. A recursive
  propagation writer remains the better long-term implementation for deeper
  roots and full non-root parent chains.
