# Stable Branch Update Refold

## Problem

Single-level branch roots can physically update an entry in place when the
replacement key stays in the same child, and can move an entry to another child
when the source remains non-empty and the target has room. A remaining stable
child-count gap is a cross-child update where the source child has one entry or
the target child is full. The total entry count does not change, so the branch
child count should remain stable, but the current code falls back to append
history instead of redistributing entries across the existing child pages.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::handler::ha_update_row()` dispatches accepted row
  replacement through the storage engine's `update_row()` implementation and
  preserves the storage engine as the owner of physical index maintenance.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::update_row()` routes
  durable rows to `mylite_storage_update_row_with_index_entry_changes()` or the
  statement-scoped variant after serializing changed index keys.
- `mariadb/sql/handler.cc` savepoint rollback paths call handlerton rollback
  hooks, so MyLite dirty-page rewrites must stay inside its statement and
  transaction journal protection model.
- `packages/mylite-storage/src/storage.c` already plans protected branch-root
  update rewrites for same-child and simple cross-child moves. It deliberately
  leaves source-emptying or target-full cross-child updates on the overlay
  fallback path.

## Design

- Keep the scope to level-`1` branch roots whose current child count matches
  the entry-count-derived child count.
- When a replacement entry belongs in a different child but the simple move
  would empty the source child or overflow the target, plan a protected refold
  across the existing branch child page ids.
- Reuse the existing branch update journal path, but protect every child leaf
  page touched by the refold plus the branch root. If the protected-page bound
  cannot cover the shape, leave the current overlay fallback in place.
- During the write, read and validate all branch child leaves, remove the
  source row id from the collected entryset, append the replacement entry, sort
  the fixed-width entries, rewrite the same child page ids with balanced leaf
  pages, and refresh the branch root fences and entry count.
- Do not reclaim or append branch child pages in this slice because the child
  count is stable.

## Non-Goals

- No multi-level branch mutation support.
- No new branch split, child removal, or file compaction behavior.
- No change to branch publication or on-disk page format.
- No attempt to implement minimal neighbor-only redistribution; this slice may
  refold all children in the protected root.

## Compatibility Impact

SQL-visible update behavior should not change except for avoiding fallback
append-history entries for this supported branch-root shape. The MariaDB
handler contract remains unchanged: `update_row()` succeeds or fails through
the existing MyLite storage result mapping, and rollback remains owned by the
existing MyLite transaction hooks.

## Single-File And Lifecycle Impact

Durable state stays in the primary `.mylite` file. The refold rewrites existing
root and leaf pages under the bounded dirty-page journal. It creates no
persistent sidecar files beyond the existing statement or transaction journals.

## Public API, Storage Routing, And Wire Protocol

No public `libmylite` API, storage-engine routing, SQL policy, or wire-protocol
change.

## Binary Size And Dependencies

Small first-party storage helpers only. No dependency or meaningful binary-size
impact.

## Tests And Verification

- Add a storage test that builds a single-level branch root, updates the only
  entry from one child into another child while the expected child count stays
  stable, and verifies exact lookup, prefix lookup, full ordered index reads,
  branch child count, rewritten leaf counts, no free-list publication, and
  file-size/header consistency.
- Cover statement rollback and stale statement/transaction journal recovery for
  the same shape.
- Run focused storage tests, storage smoke, whitespace checks, and clang-format
  diff checks for touched C files.

## Acceptance Criteria

- The stable child-count cross-child update path rewrites branch leaf pages
  instead of publishing a fallback index-entry overlay when the protected-page
  bound permits it.
- Branch readers see ordered exact, prefix, prefix-exists, and full-index
  results from the rewritten root without scanning stale replaced entries.
- Statement rollback and stale journal recovery restore the original branch
  root and child leaves.
- Docs keep broader multi-level mutation and file compaction listed as planned
  work.

## Risks

- The current journal protected-page bound means wide branch roots may still use
  the fallback path until a broader pager/journal slice raises or removes that
  limit.
- Refolding all protected child leaves is correct for this bounded shape but
  more work than the minimal redistribution a future production B-tree update
  path should use.
