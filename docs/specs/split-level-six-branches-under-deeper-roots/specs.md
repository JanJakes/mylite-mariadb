# Split Level-Six Branches Under Deeper Roots

## Problem Statement

After a full level-`7` root promotes to a bounded level-`8` root, later
no-overlay inserts can still hit a full selected level-`6` branch whose
immediate level-`7` parent has child capacity. The storage writer already knows
how to split that level-`6` branch and refresh ancestors, but planning limits
the case to level-`7` roots.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB row-write integration is unchanged. The slice only changes
  first-party MyLite maintained-index planning and storage-page publication.
- `plan_deep_branch_index_root_insert()` currently checks the root page as the
  level-`7` parent when a level-`6` parent is full, so roots deeper than level
  `7` fall back even if the actual level-`7` ancestor has room.
- `split_deep_branch_level_four_entry()` already derives
  `level_seven_parent_branch_index` from `insert->level - 7U`, rewrites that
  level-`7` page, and refreshes higher ancestors from that index upward. The
  writer needs a relaxed validation guard, not a new page layout.

## Scope

- Plan `split_level_six_branch` for roots deeper than level `7` when the
  immediate level-`7` parent has child capacity.
- Reuse the existing no-live-overlay selected-path prerequisites and journal
  bounds.
- Add fixture-backed storage coverage for a level-`8` root, append, rollback,
  and commit.

## Non-Goals

- No split of a full level-`7` branch under a level-`8` or deeper parent.
- No level-`8` or deeper root promotion.
- No recursive split propagation, branch merge, delete, or update maintenance
  change.
- No public API, SQL compatibility, file-format version, or storage-routing
  change.

## Design

When the selected level-`6` branch is full and the root level is greater than
`6`, planning should inspect the real level-`7` parent from the selected branch
path. If that level-`7` parent has spare child capacity, keep using
`split_level_six_branch`. If the root is exactly level `7` and that parent is
full, keep the level-`7` root promotion path. If a non-root level-`7` parent is
full, stay on the explicit append-tail fallback until a later slice splits
level-`7` parents under deeper roots.

The writer can keep the existing split mechanics: rewrite the selected
level-`6` page as the left half, append one right level-`6` sibling, rewrite the
level-`7` parent child list with both halves, and refresh ancestors up to the
root.

## Compatibility Impact

No compatibility surface changes. This removes an internal append-tail fallback
for an already supported fixed-width raw-index shape.

## Affected MariaDB Subsystems

No upstream MariaDB subsystem changes.

## DDL Metadata Routing Impact

No DDL metadata routing change. The catalog index-root record keeps pointing at
the same root page.

## Single-File And Recovery Impact

All durable state remains in the primary `.mylite` file. Statement rollback and
stale-journal recovery must restore the old level-`8` root, level-`7` parent,
selected level-`6` branch, lower selected path, and original file size.

## Public API, File Format, And Routing Impact

No public C API, file-format version, storage-engine routing, or wire-protocol
integration change. Existing branch-page encoding is reused.

## Build, Size, And Dependencies

No new dependencies, generated sources, or binary-size-sensitive components.

## Test Plan

- Add a fixture-backed storage test that builds a valid level-`8` root with a
  level-`7` selected child that has room and a full selected level-`6` branch.
- Assert public append rewrites the selected level-`6` branch as the left half,
  appends the right level-`6` sibling, increments the level-`7` parent child
  count, keeps the root at level `8`, and preserves exact/prefix/indexed-row
  reads for the inserted row.
- Assert statement rollback restores the root level and child count, level-`7`
  parent child count, selected level-`6` child count, file size, and lookup
  absence.
- Run storage unit tests, static checks, and storage smoke.

## Acceptance Criteria

- Eligible inserts under level-`8` and deeper roots split the selected level-`6`
  branch when its immediate level-`7` parent has child capacity.
- Rollback and commit preserve root metadata, branch fences, page count, and
  inserted-row lookup behavior.
- Existing level-`6` root promotion, level-`6` branch split, and level-`7` root
  promotion coverage remain passing.
- Full non-root level-`7` parents stay explicit fallback behavior.

## Risks And Open Questions

- This is still bounded split propagation. The next structural step is splitting
  full level-`7` parents under deeper roots, ideally after extracting shared
  parent child-list split helpers.
