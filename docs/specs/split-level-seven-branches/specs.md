# Split Level-Seven Branches

## Problem Statement

MyLite can now split full level-`6` branches below deeper roots when the
immediate level-`7` parent has child capacity. If that level-`7` parent is full
but its level-`8` parent has room, the same no-overlay insert still falls back
to append-tail index entries even though the required split can remain bounded
by the existing rollback journal.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB parser, optimizer, and handler integration do not change. The slice
  is limited to first-party MyLite maintained-index planning and storage-page
  publication.
- `plan_deep_branch_index_root_insert()` now finds the actual level-`7` parent
  on deeper selected paths, but still falls back when that parent is full and
  non-root.
- `split_deep_branch_level_four_entry()` already materializes the expanded
  level-`7` child list for level-`7` root promotion. The non-root case can reuse
  that list, rewrite the selected level-`7` parent as the left half, append one
  right level-`7` sibling, update the level-`8` parent child list, and refresh
  higher ancestors.

## Scope

- Add a maintained-index insert plan flag for splitting a full level-`7` branch
  under a level-`8` or deeper parent with child capacity.
- Reuse the existing no-live-overlay selected-path prerequisites and journal
  bounds.
- Add fixture-backed storage coverage for a level-`8` root, append, rollback,
  and commit.

## Non-Goals

- No split of a full level-`8` branch under a level-`9` or deeper parent.
- No level-`8` or deeper root promotion.
- No general recursive split propagation, branch merge, delete, or update
  maintenance change.
- No public API, SQL compatibility, file-format version, or storage-routing
  change.

## Design

When the selected level-`6` branch and its level-`7` parent are full, planning
should inspect the immediate level-`8` parent. If that parent has spare child
capacity, set `split_level_seven_branch`. If the full level-`7` parent is the
root, keep the existing level-`7` root promotion path. If the level-`8` parent
is also full, stay on the explicit append-tail fallback until a later slice
splits higher parents.

The writer first performs the lower split already used by level-`6` branch
splits. It then splits the expanded level-`7` child list into left and right
level-`7` pages, rewrites the selected level-`7` page as the left half, appends
the right level-`7` sibling, and rewrites the level-`8` parent child list with
both level-`7` halves. Existing ancestor-refresh logic propagates the changed
level-`8` fence upward.

## Compatibility Impact

No compatibility surface changes. This removes one internal append-tail
fallback for already supported fixed-width raw-index trees.

## Affected MariaDB Subsystems

No upstream MariaDB subsystem changes.

## DDL Metadata Routing Impact

No DDL metadata routing change. The catalog index-root record keeps pointing at
the same root page.

## Single-File And Recovery Impact

All durable state remains in the primary `.mylite` file. Statement rollback and
stale-journal recovery must restore the old root, level-`8` parent, selected
level-`7` branch, selected level-`6` branch, lower selected path, and original
file size.

## Public API, File Format, And Routing Impact

No public C API, file-format version, storage-engine routing, or wire-protocol
integration change. Existing branch-page encoding is reused.

## Build, Size, And Dependencies

No new dependencies, generated sources, or binary-size-sensitive components.

## Test Plan

- Add a fixture-backed storage test that builds a level-`8` root with child
  capacity, a full selected level-`7` child, and a full selected level-`6`
  branch below it.
- Assert public append rewrites the selected level-`6` branch as the left half,
  appends the right level-`6` sibling, rewrites the selected level-`7` branch as
  the left half, appends the right level-`7` sibling, increments the level-`8`
  root child count, and preserves exact/prefix/indexed-row reads for the
  inserted row.
- Assert statement rollback restores the root child count, selected level-`7`
  child count, selected level-`6` child count, file size, and lookup absence.
- Run storage unit tests, static checks, and storage smoke.

## Acceptance Criteria

- Eligible inserts split a full non-root level-`7` branch when its level-`8`
  parent has child capacity.
- Rollback and commit preserve root metadata, branch fences, page count, and
  inserted-row lookup behavior.
- Existing level-`6` root promotion, level-`6` branch split, level-`7` root
  promotion, and deeper level-`6` branch split coverage remain passing.
- Full level-`8` parents stay explicit fallback behavior.

## Risks And Open Questions

- This is still bounded split propagation. Splitting full level-`8` parents and
  eventually replacing the fixed-depth planner with recursive propagation remain
  future work.
