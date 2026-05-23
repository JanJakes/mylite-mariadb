# Branch Final Leaf Reclamation

## Problem

Final-child removal and branch-root collapse deliberately leave old branch leaf
pages unreferenced. That is correct for reader safety, but it accumulates
durable dead pages and prevents the existing catalog free-list from reusing
those pages for future storage allocations.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- MyLite already has a durable single-file free-list page format and catalog
  page-run allocation/reclamation helpers.
- Final-child removal already validates the removed final leaf and rewrites the
  branch root under the statement or transaction recovery journal.
- Reclaiming the removed leaf as a one-page free-list run only requires
  protecting that leaf page, encoding a free-list node there, and publishing the
  header's `free_list_root_page` to that node.

## Scope

- When a final-child removal succeeds, encode the removed leaf page as a
  one-page free-list run whose next root points at the previous free-list root.
- Update `header.free_list_root_page` before publishing the statement header.
- Protect the removed leaf page in the same preplanned dirty-page journal used
  for the branch root.
- Keep reclamation limited to final child leaves removed from a single-level
  branch root.

## Non-Goals

- No reclamation for stable final-leaf deletes, interior deletes, updates, or
  inserts.
- No coalescing adjacent free-list runs.
- No general row/index page allocator beyond the existing catalog free-list
  consumer.
- No file shrinking.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. The change only
reclassifies unreferenced internal leaf pages as reusable free-list pages.

## Single-File And Lifecycle Impact

Reclaimed pages remain inside the primary `.mylite` file and become part of the
durable free-list chain. Rollback restores the original leaf bytes and header.

## Recovery Impact

Statement rollback, transaction rollback, and stale-journal recovery must
restore both the branch root and the reclaimed leaf page, plus the previous
header free-list root.

## Test Plan

- Extend final-child removal and branch-collapse coverage to assert the header
  free-list root points at reclaimed final leaf pages after commit.
- Cover statement rollback plus stale statement and transaction recovery so
  reclaimed leaves and header free-list roots are restored.
- Verify collapsed roots and live reads still ignore reclaimed leaves.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Removed final child leaves are published as one-page free-list runs on commit.
- Rollback and stale recovery restore leaf bytes and the prior header
  `free_list_root_page`.
- Existing catalog free-list validation accepts the reclaimed leaf.
- Docs distinguish reclamation from file shrinking and run coalescing.

## Implementation Notes

- Final-child branch delete planning now protects both the branch root and the
  removed leaf page before either page is rewritten.
- Successful final-child removal encodes the removed leaf page as a
  one-page free-list run whose `next_root_page` preserves the previous
  free-list root, then publishes the removed leaf as the new
  `header.free_list_root_page`.
- Branch-root collapse uses the same reclamation path after materializing the
  remaining live entries into the root page.
- The storage tests assert both header publication and the encoded free-list
  node contents for final-child removal and branch-root collapse, including
  rollback and stale-journal recovery restoration.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Risks And Follow-Ups

- The free-list remains a simple run stack without coalescing.
- Only catalog publication currently consumes the free-list; row/index page
  reuse remains future work.
