# Split Level-Eight Branches

## Problem Statement

MyLite can now promote an exactly full level-`8` root to a bounded level-`9`
root. A later insert into a full non-root level-`8` branch below a level-`9`
parent still falls back to append-tail index entries even when that level-`9`
parent has spare child capacity.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB parser, optimizer, and handler integration do not change. The slice
  is limited to first-party MyLite maintained-index planning and storage-page
  publication after MariaDB routes the row write to the MyLite handler.
- `plan_deep_branch_index_root_insert()` now distinguishes level-`8` root
  promotion from non-root level-`8` fallback. It can inspect the immediate
  level-`9` parent on deeper selected paths.
- `split_deep_branch_level_four_entry()` already materializes the expanded
  level-`8` child list for level-`8` root promotion. The non-root case can reuse
  that list, rewrite the selected level-`8` branch as the left half, append one
  right level-`8` sibling, update the level-`9` parent child list, and refresh
  higher ancestors.

## Scope

- Add a maintained-index insert plan flag for splitting a full level-`8` branch
  under a level-`9` or deeper parent with child capacity.
- Reuse the existing no-live-overlay selected-path prerequisites and
  rollback-bounded dirty-page publication.
- Convert the full non-root level-`8` fixture from deliberate fallback coverage
  to append, rollback, and commit coverage for the split path.

## Non-Goals

- No split of a full level-`9` branch under a level-`10` or deeper parent.
- No level-`9` or deeper root promotion.
- No general recursive split propagation, branch merge, delete, or update
  maintenance change.
- No public API, SQL compatibility, file-format version, storage-engine
  routing, or wire-protocol change.

## Design

When the selected level-`6` branch, level-`7` parent, and level-`8` parent are
full, planning should inspect the immediate level-`9` parent. If that parent has
spare child capacity, set `split_level_eight_branch`. If the full level-`8`
parent is the root, keep the existing level-`8` root promotion path. If the
level-`9` parent is also full, stay on the explicit append-tail fallback until
a later slice handles higher recursive propagation.

The writer first performs the existing lower split sequence: split the selected
leaf, level-`1` through level-`5` path, selected level-`6` branch, and selected
level-`7` branch. It then splits the expanded level-`8` child list into left and
right level-`8` pages, rewrites the selected level-`8` branch as the left half,
appends the right level-`8` sibling, and rewrites the level-`9` parent child
list with both level-`8` halves. Existing ancestor-refresh logic propagates the
changed level-`9` fence upward.

## Compatibility Impact

No compatibility surface changes. This removes one internal append-tail
fallback for already supported fixed-width raw-index trees.

## Affected MariaDB Subsystems

No upstream MariaDB subsystem changes.

## DDL Metadata Routing Impact

No DDL metadata routing change. Existing index-root catalog records keep
pointing at the same root page.

## Single-File And Recovery Impact

All durable state remains in the primary `.mylite` file. Statement rollback and
stale-journal recovery must restore the old root, level-`9` parent, selected
level-`8` branch, selected level-`7` branch, selected level-`6` branch, lower
selected path, and original file size.

## Public API, File Format, And Routing Impact

No public C API, file-format version, storage-engine routing, or wire-protocol
integration change. Existing branch-page encoding is reused.

## Build, Size, And Dependencies

No new dependencies, generated sources, or binary-size-sensitive components.

## Test Plan

- Add or update fixture-backed storage coverage for a level-`9` root with child
  capacity, a full selected level-`8` child, a full selected level-`7` child,
  and a full selected level-`6` branch below it.
- Assert public append rewrites the selected level-`8`, level-`7`, and level-`6`
  branches as left halves, appends right level-`8`, level-`7`, and level-`6`
  siblings, increments the level-`9` root child count, and preserves
  exact/prefix/indexed-row reads for the inserted row.
- Assert statement rollback restores the root child count, selected level-`8`
  child count, selected level-`7` child count, selected level-`6` child count,
  file size, and lookup absence.
- Run storage unit tests, static checks, and storage smoke.

## Acceptance Criteria

- Eligible inserts split a full non-root level-`8` branch when its level-`9`
  parent has child capacity.
- Rollback and commit preserve root metadata, branch fences, page count, and
  inserted-row lookup behavior.
- Existing level-`7` branch split and level-`8` root promotion coverage remain
  passing.
- Full non-root level-`9` parents stay explicit fallback behavior.

## Risks And Open Questions

- This remains bounded fixed-depth propagation. The next higher split would be
  better served by factoring the repeated parent split/publish pattern or by
  replacing it with general recursive propagation.
