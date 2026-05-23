# Branch Child-Count Delete Collapse

## Problem

Branch roots can collapse to a maintained single-page root when deleting a
one-entry child leaves a small enough live entryset. The equivalent
child-count-reducing delete from a multi-entry child still falls back when the
post-delete entries would fit a maintained root page.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB InnoDB uses broader B-tree compression/merge machinery in
  `mariadb/storage/innobase/include/btr0btr.h` and
  `mariadb/storage/innobase/btr/btr0btr.cc`. MyLite's first-party storage can
  keep this slice narrower because the target is a bounded single-level branch
  root collapse.
- `remove_branch_index_child_entry()` already calls
  `try_collapse_branch_index_root_after_child_removal()` for one-entry child
  removals.
- `plan_branch_index_root_delete()` now has the exact decision point needed for
  multi-entry child-count-reducing deletes: `removes_child` is true and the
  source leaf contains more than one entry.

## Scope

- Plan a physical branch collapse when:
  - the branch root is level `1`;
  - deleting the row reduces the expected child count by one;
  - the source child leaf contains the deleted row and has more than one entry;
  - the post-delete entry count fits a maintained single-page root;
  - there is no live append-tail overlay after the current branch leaves;
  - the branch root, old final child page, and any current free-list root
    mutation fit the bounded protected-page journal plan.
- Materialize branch child entries, remove the deleted row, and encode the root
  page as a maintained index root.
- Reclaim the old final child page through the existing durable free-list path.
- Preserve fallback behavior for unsupported shapes.

## Non-Goals

- No arbitrary-depth B-tree balancing.
- No multi-level branch writer maintenance.
- No reclamation of every superseded child page; this follows the existing
  branch-collapse behavior and reclaims only the removed/final child page.
- No collapse across live append-tail overlays; those keep the existing
  fallback path so live overlay entries are not hidden by the root rewrite.
- No SQL-visible behavior, public API, storage-engine routing, or page-format
  change.

## Compatibility Impact

No MariaDB-visible SQL behavior changes. Eligible deletes avoid append-tail
index visibility work while preserving exact, prefix, indexed-row, and
full-index read results.

## Single-File And Lifecycle Impact

All durable state remains inside the primary `.mylite` file. The journal
protects the root page and reclaimed final child page, plus the current
free-list root when reclaim coalescing mutates it. Rollback and stale recovery
restore the previous branch root, free-list root, and logical visibility.

## File-Format Impact

No file-format change. The slice rewrites an existing branch root page as an
existing maintained-root page type.

## Test Plan

- Add a storage unit test that builds a two-child branch root, deletes from a
  multi-entry child, and verifies the root collapses to a maintained root page.
- Verify exact lookup, prefix lookup, prefix-exists, indexed-row materialization,
  and full index reads after collapse.
- Verify no append-tail index-entry fallback is needed by checking expected
  page-count growth and root page type.
- Preserve existing append-tail overlay fallback behavior when branch collapse
  would hide live overlay entries.
- Cover statement rollback, stale statement recovery, and stale transaction
  recovery for the collapse path.
- Preserve branch refold, arbitrary child removal, same-child delete, split,
  root promotion, and storage-smoke coverage.
- Run:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --preset storage-smoke-dev --output-on-failure
```

## Acceptance Criteria

- Eligible child-count-reducing deletes from multi-entry child leaves with no
  live append-tail overlay collapse the branch root to a maintained root instead
  of using append-tail index-entry fallback.
- Branch exact lookup, prefix lookup, prefix-exists, indexed-row materialization,
  and full-index reads remain correct after collapse.
- Rollback and stale recovery restore the previous branch root, free-list root,
  and logical visibility.
- Unsupported collapse shapes keep the existing fallback behavior.

## Risks And Follow-Ups

- Superseded non-final child leaves remain orphaned until broader free-space
  reclamation exists, matching current collapse behavior.
- Branches with live append-tail overlay still need broader overlay-aware
  compaction before they can use this collapse path.
- Future work still needs multi-level writer maintenance, broader branch
  merge/redistribution policy, arbitrary-chain free-list coalescing, and
  unbounded branch cursors.
