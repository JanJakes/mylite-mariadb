# Promote Level-Nine Roots

## Problem Statement

MyLite can split a full non-root level-`8` branch when its level-`9` parent has
child capacity. If that level-`9` parent is the root and is also exactly full,
the same insert still falls back to append-tail index-entry pages even though a
bounded promotion to a level-`10` root can preserve maintained branch pages.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB parser, optimizer, and handler behavior do not change. The slice is
  limited to first-party MyLite raw-index maintenance after MariaDB routes the
  row write to the MyLite handler.
- `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES` is `16`, and the
  maintained deep-branch planner caps branch levels at
  `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES - 1`. A level-`9` root
  selected path protects nine branch pages plus the selected leaf, which stays
  inside the existing rollback journal protected-page limit.
- `plan_deep_branch_index_root_insert()` currently returns fallback when the
  selected level-`8` branch and its level-`9` root parent are both full.
- `split_deep_branch_level_four_entry()` already builds the expanded
  level-`8` child list, can split that list into left/right level-`8` pages,
  and has a helper for expanding the level-`9` root child list with those two
  level-`8` halves.

## Scope

- Add a maintained-index insert plan flag for promoting an exactly full
  level-`9` root to a bounded level-`10` root.
- Reuse the existing selected-path, no-live-overlay prerequisites.
- Reuse the parent split helper to expand the level-`9` root child list,
  split it into two appended level-`9` child pages, and rewrite the original
  root page as a level-`10` root with two children.
- Add fixture-backed append, rollback, and commit coverage.

## Non-Goals

- No split of full non-root level-`9` branches under level-`10` or deeper
  parents.
- No level-`10` or deeper root promotion.
- No general recursive propagation.
- No public API, SQL compatibility, file-format version, storage-engine
  routing, or wire-protocol change.

## Design

When the selected leaf, level-`1` through level-`7` ancestors, selected
level-`8` branch, and level-`9` root are full, planning should set
`promote_level_nine_root` instead of falling back. The existing live-overlay
guard remains in force so the promotion does not hide append-tail index-entry
pages.

The writer keeps the lower split sequence unchanged through the selected
level-`7` branch. It then rewrites the selected level-`8` branch as the left
half and appends the right level-`8` sibling. It expands the level-`9` root
child list by replacing the selected level-`8` child with those two level-`8`
halves, reads sibling level-`8` entry counts, splits the expanded list into
left and right level-`9` pages, appends both level-`9` pages, and rewrites the
original root page as a level-`10` root.

## Compatibility Impact

No compatibility surface changes. This removes one internal append-tail
fallback for already supported fixed-width raw-index trees.

## DDL Metadata Routing Impact

No DDL metadata routing change. Existing index-root catalog records keep
pointing at the same root page.

## Single-File And Recovery Impact

All durable state remains in the primary `.mylite` file. Statement rollback and
stale-journal recovery must restore the original level-`9` root, selected
level-`8` branch, selected level-`7` branch, selected level-`6` branch, lower
selected path, and original file size.

## Public API, File Format, And Routing Impact

No public C API, file-format version, storage-engine routing, or wire-protocol
integration change. Existing branch-page encoding is reused.

## Build, Size, And Dependencies

No new dependencies or generated sources. The change adds a bounded plan flag
and a small amount of writer logic inside the existing storage module.

## Test Plan

- Add fixture-backed storage coverage for a full level-`9` root with a full
  selected level-`8` child, full selected level-`7` child, and full selected
  level-`6` branch below it.
- Assert append promotes the root to level `10`, appends left/right level-`9`
  child pages, rewrites the selected level-`8`, level-`7`, and level-`6`
  branches as left halves, and preserves exact/prefix/indexed-row reads for
  the inserted row.
- Assert statement rollback restores root level, child counts, branch pages,
  file size, and lookup absence.
- Run storage unit tests, static checks, and storage smoke.

## Acceptance Criteria

- Eligible full level-`9` roots promote to bounded level-`10` roots.
- Full level-`9` roots no longer append fallback index-entry pages for this
  exact eligible shape.
- Existing level-`8` branch split coverage remains correct. Full non-root
  level-`9` parents remained explicit fallback behavior for this slice and are
  covered by the follow-up split-level-nine-branches slice.
- Rollback and commit preserve root metadata, branch fences, page count, and
  inserted-row lookup behavior.

## Risks And Open Questions

- This keeps fixed-depth propagation. The next higher non-root level-`9` split
  belongs in its own follow-up slice before broader recursive propagation.
