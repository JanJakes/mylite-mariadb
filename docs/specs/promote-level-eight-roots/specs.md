# Promote Level-Eight Roots

## Problem Statement

MyLite can split full level-`7` branches when an existing level-`8` parent has
child capacity. If the selected level-`8` root itself is child-cell-full, the
same no-overlay insert still falls back to append-tail index entries even
though the bounded promotion can remain within the maintained-level and
rollback-journal limits.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB parser, optimizer, and handler integration do not change. The slice
  is limited to first-party MyLite maintained-index planning and storage-page
  publication after MariaDB routes the row write to the MyLite handler.
- `plan_deep_branch_index_root_insert()` now distinguishes level-`7` branch
  splits under level-`8` parents from level-`7` root promotion, but still falls
  back when the selected root is exactly level `8` and its child list is full.
- `split_deep_branch_level_four_entry()` already materializes the expanded
  level-`7` child list for level-`7` root promotion and non-root level-`7`
  branch splits. Level-`8` root promotion needs one more bounded parent split:
  materialize the expanded level-`8` child list, split it into two appended
  level-`8` branch pages, and rewrite the old root as a level-`9` root.

## Scope

- Add a maintained-index insert plan flag for promoting an exactly full
  level-`8` root to a bounded level-`9` root.
- Reuse the existing no-live-overlay selected-path prerequisites and
  rollback-bounded dirty-page publication.
- Add fixture-backed storage coverage for append, statement rollback, and
  commit.

## Non-Goals

- No split of a full non-root level-`8` branch under a level-`9` or deeper
  parent.
- No level-`9` or deeper root promotion.
- No general recursive split propagation, branch merge, delete, or update
  maintenance change.
- No public API, SQL compatibility, file-format version, storage-engine
  routing, or wire-protocol change.

## Design

When the selected level-`6` branch, level-`7` parent, and level-`8` root are
full, planning should set `promote_level_eight_root` if the root can grow within
`MYLITE_STORAGE_INDEX_BRANCH_MAX_MAINTAINED_LEVEL`. If the full level-`8` page
is not the root, the insert stays on the explicit append-tail fallback until a
later slice splits level-`8` branches under level-`9` parents.

The writer first performs the existing lower split sequence: split the selected
leaf, level-`1` through level-`5` path, selected level-`6` branch, and selected
level-`7` branch. It then splits the expanded level-`8` root child list into
left and right level-`8` pages, appends those two pages, and rewrites the old
root page as a level-`9` root with two children. Sibling level-`7` branch entry
counts must be read while building the expanded level-`8` list so the new
level-`8` pages keep page-owned entry counts accurate.

## Compatibility Impact

No compatibility surface changes. This removes one internal append-tail
fallback for already supported fixed-width raw-index trees.

## Affected MariaDB Subsystems

No upstream MariaDB subsystem changes.

## DDL Metadata Routing Impact

No DDL metadata routing change. The catalog index-root record keeps pointing at
the same root page; that page changes from level `8` to level `9`.

## Single-File And Recovery Impact

All durable state remains in the primary `.mylite` file. Statement rollback and
stale-journal recovery must restore the old level-`8` root, selected level-`7`
branch, selected level-`6` branch, lower selected path, and original file size.

## Public API, File Format, And Routing Impact

No public C API, file-format version, storage-engine routing, or wire-protocol
integration change. Existing branch-page encoding is reused.

## Build, Size, And Dependencies

No new dependencies, generated sources, or binary-size-sensitive components.

## Test Plan

- Add a fixture-backed storage test that builds a full valid level-`8` root
  with a full selected level-`7` child and full selected level-`6` branch below
  it.
- Assert public append rewrites the old root to level `9`, appends two
  level-`8` children, appends the right level-`7` sibling, rewrites the selected
  level-`7` and level-`6` branches as left halves, and preserves
  exact/prefix/indexed-row reads for the inserted row.
- Assert statement rollback restores the original root level, root child count,
  selected level-`7` child count, selected level-`6` child count, file size, and
  lookup absence.
- Assert a full non-root level-`8` branch under a level-`9` root keeps the
  append-tail fallback path until non-root level-`8` split propagation exists.
- Run storage unit tests, static checks, and storage smoke.

## Acceptance Criteria

- Covered inserts into an exactly full level-`8` root promote to a bounded
  level-`9` root instead of publishing an append-tail index-entry fallback.
- Rollback and commit preserve root metadata, branch fences, page count, and
  inserted-row lookup behavior.
- Existing level-`7` root promotion and level-`7` branch split coverage remain
  passing.
- Full non-root level-`8` parents stay explicit fallback behavior.

## Risks And Open Questions

- This remains bounded fixed-depth propagation. The repeated parent split logic
  should be factored before extending many more levels or replacing the planner
  with general recursive split propagation.
