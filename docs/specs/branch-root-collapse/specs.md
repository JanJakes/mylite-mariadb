# Branch Root Collapse

## Problem

After final-child removals, a single-level branch root can shrink to one child
and a live entryset that fits in a single maintained root page. Keeping that
shape as a branch root preserves unnecessary child navigation and leaves a
broader branch-maintenance surface for later row DML.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `read_branch_index_root_entries()` can materialize a branch root as a live
  entryset by reading the branch base pages and applying append-tail row-state
  and index-entry overlays.
- `encode_maintained_index_root_page_from_entryset()` already publishes a
  sorted single-page maintained root from an entryset.
- Final-child removal already validates the removed leaf but leaves it
  unreferenced. Collapse can therefore rewrite only the root page and leave all
  old leaf pages as durable skip pages.

## Scope

- Collapse a single-level branch root to a maintained root when a final-child
  removal leaves a live entryset that fits in one maintained root page.
- Build the collapse root from the live branch entryset plus the current delete
  applied to that entryset, so prior row-state-hidden entries do not become
  root-resident again.
- Keep old branch child leaves unmodified and unreferenced.
- Preserve the current row-state delete overlay and normal rollback behavior.

## Non-Goals

- No collapse on update or insert paths.
- No free-list publication, page reclamation, or file shrinking.
- No multi-level branch collapse.
- No branch-to-root collapse when the live entryset does not fit in one
  maintained root page.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. This only
changes the physical index root representation for a maintained MyLite storage
shape.

## Single-File And Lifecycle Impact

The root page is rewritten in place under the existing dirty-page journal
protection. Old branch leaf pages remain inside the primary `.mylite` file and
are ignored by maintained-root readers unless later referenced by future
free-list/reclamation work.

## Recovery Impact

Rollback and stale-journal recovery restore the old branch root page. The
current delete row-state page remains append-only and is truncated on rollback.

## Test Plan

- Extend the branch-root overflow test after repeated final-child removals so
  the remaining live entryset fits in a single maintained root.
- Verify the root page type changes from branch to maintained root on commit.
- Cover statement rollback plus stale statement and transaction recovery for
  the collapsing delete.
- Verify exact lookup, full index reads, row materialization, and later
  maintained-root update/delete behavior after collapse.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Eligible final-child removal collapses to a maintained root only when the live
  post-delete entryset fits the root page format.
- Prior row-state-hidden entries are not reintroduced into the collapsed root.
- Unsupported collapse shapes keep the branch-root removal behavior.
- Docs and roadmap distinguish collapse from later page reclamation.

## Implementation Notes

- The final-child removal writer now attempts collapse before rewriting the
  branch child list.
- Collapse reads all remaining branch child leaves, applies existing tail
  row-state and index-entry overlays, removes the current deleted row id, and
  encodes a maintained root when the live entryset fits.
- Old child leaves are not modified. They become unreferenced skip pages until
  future reclamation work.
- If collapse is not possible, the delete keeps the previous branch-child
  removal behavior.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Risks And Follow-Ups

- Old branch leaf pages remain durable dead space until free-list/reclamation
  work.
- Collapse currently depends on rebuilding the live branch entryset during the
  delete, which is acceptable for the narrow shrink path but not a substitute
  for general B-tree merge/redistribution.
