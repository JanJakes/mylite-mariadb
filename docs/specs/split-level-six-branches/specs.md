# Split Level-Six Branches

## Problem Statement

MyLite can promote an exactly full level-`6` root to a bounded level-`7` root,
but a later insert that fills a selected level-`6` branch under an existing
level-`7` parent still falls back to append-tail index entries. That leaves the
deepest currently supported branch shape incomplete for the no-overlay insert
path.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB still reaches MyLite index maintenance through the handler row-write
  path; the affected implementation is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`.
- `plan_deep_branch_index_root_insert()` already identifies leaf, level-`1`,
  level-`2`, level-`3`, level-`4`, level-`5`, and level-`6` full-child shapes.
  The current full level-`6` branch case promotes only when the root itself is
  level `6`; a level-`7` parent with child capacity returns to fallback.
- `split_deep_branch_level_four_entry()` already has the page-building pieces
  needed for the lower levels and for level-`6` root promotion. The missing
  shape reuses the same lower-page split, rewrites the selected level-`6`
  branch as the left branch, appends one right level-`6` sibling, and inserts
  that sibling into the level-`7` parent.

## Scope

- Add a maintained-index insert plan flag for splitting a full level-`6` branch
  under an existing level-`7` parent that still has child capacity.
- Keep the implementation to the no-live-overlay, high-key or selected-path
  insert shape already covered by deep branch insert planning.
- Add fixture-backed storage coverage that constructs the level-`7` parent
  shape directly and then exercises public append, rollback, and commit.

## Non-Goals

- No recursive split propagation above level `7`.
- No level-`7` root promotion to level `8`.
- No branch merge or broader delete/update maintenance change.
- No public API, SQL compatibility, file-format version, or storage-routing
  change.

## Design

Extend deep branch insert planning with `split_level_six_branch`. The planner
sets it only when the selected level-`6` child is full, its level-`7` parent has
space for one more child, and the existing no-overlay lower split prerequisites
are satisfied.

The writer reuses the level-`1` through level-`5` split construction already
used by level-`6` root promotion. Instead of allocating two new level-`6`
branches and rewriting the old root as level `7`, it rewrites the selected
level-`6` branch in place as the left half, appends one new right level-`6`
branch, and rewrites the level-`7` parent child list with both level-`6`
children. The row page plus index pages add eight pages total for the covered
shape.

## Compatibility Impact

No compatibility surface changes. This removes one internal append-tail
fallback for an already supported insert shape and keeps externally visible row
and index lookup behavior unchanged.

## Affected MariaDB Subsystems

No upstream MariaDB parser, optimizer, handler API, or SQL layer changes. The
slice is limited to first-party MyLite maintained-index storage maintenance
after MariaDB has already routed the row write into the MyLite handler.

## DDL Metadata Routing Impact

No DDL metadata routing change. Existing index-root catalog records continue to
point at the same root page; only the contents of the maintained branch tree are
rewritten.

## Single-File And Recovery Impact

The mutation stays inside the existing `.mylite` file and uses the existing
statement journal page-protection path. Rollback must restore the level-`7`
parent, selected level-`6` branch, selected path pages, and original file size.

## Public API, File Format, And Routing Impact

No public C API, file-format version, storage-engine routing, or wire-protocol
integration change. The existing branch-page format is reused.

## Build, Size, And Dependencies

No new dependencies, build targets, generated sources, or binary-size-sensitive
components.

## Test Plan

- Add a fixture-backed storage test that builds a sparse valid level-`7` root
  with a full selected level-`6` child and parent child capacity.
- Assert public append rewrites the parent to one additional child, rewrites the
  selected level-`6` child as the left half, appends the right level-`6`
  sibling, and preserves exact/prefix/indexed-row reads for the inserted row.
- Assert statement rollback restores the original root child count, selected
  level-`6` child count, file size, and lookup absence.
- Run storage unit tests, static checks, and storage smoke.

## Acceptance Criteria

- Covered inserts below an existing level-`7` root split the selected full
  level-`6` branch instead of publishing an append-tail index-entry fallback.
- Rollback and commit paths preserve root metadata, branch fences, page count,
  and inserted-row lookup behavior.
- The broader recursive split cases remain explicit fallback behavior.

## Risks And Open Questions

- This deliberately does not solve level-`7` parent overflow. A later recursive
  propagation slice should generalize the parent insertion pattern instead of
  adding another one-off upper level.
