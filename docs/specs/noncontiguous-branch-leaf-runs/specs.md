# Noncontiguous Branch Leaf Runs

## Problem

Single-level branch roots currently store child page ids, but the reader still
requires the children to be physically contiguous at `root + 1`. That works for
copy-rebuilt snapshots appended as one page run, but it blocks a stable-root
split path where an existing root page would become a branch page and point to
newly appended child leaves elsewhere in the file.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- Branch pages already store a child page id per high `(key, row_id)` fence.
- `read_index_branch_leaf_run_root()` currently validates that the first child
  is `definition_root_page + 1` and every later child is contiguous.
- `read_index_leaf_run_page()` and exact/prefix branch lookup convert a logical
  page offset back to `first_page_id + offset`, which prevents noncontiguous
  child ids from being used even though the branch page encodes them.

## Scope

- Preserve the existing contiguous rebuilt branch-root layout.
- Teach branch-rooted leaf runs to keep child page ids in branch-cell order.
- Read branch-rooted leaf pages by logical child offset rather than by
  `first_page_id + offset`.
- Keep append-tail overlay start at the first page after the highest child page
  id for single-level branch roots.
- Cover a branch root whose child ids are intentionally non-monotonic while the
  leaf-page key order remains branch-cell ordered.

## Non-Goals

- No root split writer in this slice.
- No branch-page split, merge, rebalance, deletion, or free-list reuse.
- No multi-level branch tree.
- No variable-width or collation-aware branch keys.
- No SQL-visible behavior change.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. Exact, prefix,
and full reads over existing rebuilt branch roots should return the same
results. This is a storage-internal prerequisite for future maintained-root
split work.

## Single-File And Lifecycle Impact

All branch and leaf pages remain durable pages inside the primary `.mylite`
file. No companion files are introduced. Existing append-tail overlays still
scan pages after the immutable branch snapshot.

## File-Format Impact

No new page type or field. The reader starts honoring the child page ids already
present in `TABLE_INDEX_BRANCH` page type `13`.

## Test Plan

- Build a multi-page branch-rooted fixed-width index, then rewrite the branch
  root and child leaf page ids so the branch child order is key-sorted but the
  physical page ids are non-monotonic.
- Verify exact lookup, full index reads, and prefix reads follow branch child
  ids rather than assuming `root + offset`.
- Keep existing contiguous branch-root coverage passing.
- Run:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure
ctest --preset storage-smoke-dev --output-on-failure
```

## Implementation Notes

- `mylite_storage_index_leaf_run` keeps branch child page ids in branch-cell
  order for single-level branch roots.
- Branch-rooted leaf reads map logical child offsets through that page-id list;
  contiguous leaf runs still use `first_page_id + offset`.
- Exact and prefix branch lookup resolve branch-selected child page ids back to
  logical offsets before reading the target leaf page.
- The append-tail overlay for branch-rooted runs begins after the highest child
  page id recorded in the branch root.

## Verification Results

Passed on 2026-05-23:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure
ctest --preset storage-smoke-dev --output-on-failure
```

## Acceptance Criteria

- Branch-rooted leaf runs can read child leaves through branch-cell child page
  ids when those ids are not contiguous with the root page.
- Existing contiguous branch roots remain valid and keep append-tail overlay
  behavior.
- Exact, prefix, and full branch-root reads remain ordered and validated.
- Roadmap wording reflects this as a prerequisite for stable-root split work,
  not full split/merge support.

## Risks And Follow-Ups

- The branch-root leaf-run struct grows by one fixed child-id array sized to the
  maximum possible single-page branch fanout. This is acceptable for stack use
  in the current storage code, but multi-level trees should move to a narrower
  cursor/page object.
- Future split writers still need root conversion, dirty-page protection,
  recovery tests, and delete/merge policy.
