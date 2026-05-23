# Free-List Chain Allocation

## Problem

MyLite can now coalesce reclaimed page runs with adjacent runs anywhere in the
free-list chain, but catalog publication still allocates only from the current
free-list root run. If the root run is too small and a later run is large
enough, storage appends new pages instead of reusing available space.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage behavior; MariaDB's engine-specific
  free-space managers are not the source authority for the `.mylite` page
  format.
- `allocate_catalog_page_run()` currently reads only
  `header->free_list_root_page`.
- Catalog free-list reuse is disabled while active statements, active read
  statements, or active read snapshots exist, so this slice only affects
  non-active catalog publication.

## Scope

- Scan the linked free-list chain for the first run large enough to satisfy a
  catalog page-run allocation.
- Continue allocating from the tail of the selected run so the free-list root
  page id remains the run start when a partial run remains.
- When the selected run is exhausted:
  - move `header->free_list_root_page` to the next run if the exhausted run is
    the root;
  - otherwise rewrite the previous free-list node to bypass the exhausted run.
- Preserve existing append-at-EOF behavior when no free run is large enough.
- Reject corrupt free-list chains through existing free-list page validation and
  a bounded traversal guard.

## Non-Goals

- No best-fit or sorted free-list allocation policy.
- No allocation from free-list chains during active statements.
- No file truncation or page relocation.
- No page-format change.

## Compatibility Impact

No SQL-visible behavior changes. The slice only improves internal page reuse for
catalog publication.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. Reused pages remain
inside the file and become the new catalog-chain location chosen by catalog
publication.

## File-Format Impact

No file-format change. Existing `MYLFRE1` pages are rewritten in place when a
run is partially consumed or when a predecessor bypasses an exhausted run.

## Test Plan

- Add storage hook coverage for:
  - partial allocation from a non-root run when the root is too small;
  - exact allocation from a non-root run that removes that free-list node;
  - append fallback when no free-list run is large enough.
- Preserve existing root-run allocation, reclamation, corruption, and
  storage-smoke coverage.
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

- Catalog allocation reuses a suitable non-root free-list run when the root run
  is too small.
- Partial non-root reuse shrinks the selected run and preserves the chain link.
- Exact non-root reuse removes the selected node by updating its predecessor.
- No-suitable-run allocation keeps appending at EOF.
- Existing root-run reuse behavior remains intact.

## Risks And Follow-Ups

- First-fit chain order remains recency-based, not page-id sorted or best-fit.
- Active-statement catalog free-list reuse remains disabled.
- Broader file shrinking and page compaction remain planned.
