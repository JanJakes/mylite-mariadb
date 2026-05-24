# Promote Level-Seven Roots

## Problem Statement

MyLite can split a full level-`6` branch when an existing level-`7` parent has
child capacity. If that level-`7` root is also child-cell-full, the same
no-overlay insert still falls back to append-tail index entries. That leaves the
next root-promotion case in the maintained deep branch path uncovered.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB reaches this path through the existing MyLite handler row-write
  integration. No MariaDB parser, optimizer, or handler API change is needed.
- Before this slice, `plan_deep_branch_index_root_insert()` distinguished
  level-`6` root promotion from level-`6` branch splits under level-`7` parents,
  but a full level-`7` root still returned to fallback.
- `split_deep_branch_level_four_entry()` already builds the lower split through
  a rewritten left level-`6` branch plus an appended right level-`6` sibling.
  Promoting the full level-`7` root requires splitting that expanded level-`7`
  child list into two appended level-`7` branch pages and rewriting the old root
  as a level-`8` root.

## Scope

- Add a maintained-index insert plan flag for promoting an exactly full
  level-`7` root to a bounded level-`8` root.
- Reuse the existing no-live-overlay selected-path prerequisites.
- Add fixture-backed storage coverage for public append, rollback, and commit.

## Non-Goals

- No recursive split propagation under existing level-`8` or deeper parents.
- No level-`8` root promotion to level `9`.
- No branch merge, delete, or update maintenance change.
- No public API, SQL compatibility, file-format version, or storage-routing
  change.

## Design

Extend deep branch insert planning with `promote_level_seven_root`. The planner
sets it only when the selected level-`6` child is full, the root is exactly
level `7`, the root child list is full, and the root can grow within the current
journal-bounded maintained level limit.

The writer first performs the same lower split as the level-`6` branch split
path: rewrite the selected level-`6` page as the left half and append one right
level-`6` page. It then builds the expanded level-`7` root child list, reads
sibling level-`6` entry counts for accurate page-owned counts, appends left and
right level-`7` branch pages, and rewrites the old root page as a level-`8` root
with two children.

The covered mutation appends the row page, split leaf, right level-`1` through
level-`6` branch pages, and two level-`7` branch pages.

## Compatibility Impact

No compatibility surface changes. The external row and index lookup behavior is
unchanged; this removes one internal append-tail fallback for an already
supported index shape.

## Affected MariaDB Subsystems

No upstream MariaDB subsystem changes. The slice is limited to first-party
MyLite storage maintenance after MariaDB has routed the row write to MyLite.

## DDL Metadata Routing Impact

No DDL metadata routing change. The catalog index-root record keeps pointing at
the same root page; that page changes from level `7` to level `8`.

## Single-File And Recovery Impact

All durable state remains in the primary `.mylite` file. The statement journal
must restore the old level-`7` root, selected level-`6` branch, selected lower
path, and original file size on rollback or stale-journal recovery.

## Public API, File Format, And Routing Impact

No public C API, file-format version, storage-engine routing, or wire-protocol
integration change. Existing branch-page encoding is reused.

## Build, Size, And Dependencies

No new dependencies, generated sources, or binary-size-sensitive components.

## Test Plan

- Add a fixture-backed storage test that builds a full valid level-`7` root
  with a full selected level-`6` child.
- Assert public append rewrites the old root to level `8`, appends two
  level-`7` children, rewrites the selected level-`6` branch as the left half,
  appends the right level-`6` sibling, and preserves exact/prefix/indexed-row
  reads for the inserted row.
- Assert statement rollback restores the original root level, root child count,
  selected level-`6` child count, file size, and lookup absence.
- Run storage unit tests, static checks, and storage smoke.

## Acceptance Criteria

- Covered inserts into an exactly full level-`7` root promote to a bounded
  level-`8` root instead of publishing an append-tail index-entry fallback.
- Rollback and commit preserve root metadata, branch fences, page count, and
  inserted-row lookup behavior.
- Existing level-`6` root promotion and level-`6` branch split coverage remain
  passing.
- Broader recursive split-propagation cases stay explicit fallback behavior.

## Risks And Open Questions

- This is still a bounded promotion, not the final recursive B-tree split
  implementation. The next broader slice should factor the repeated parent
  child-list split pattern before adding deeper parent propagation.
