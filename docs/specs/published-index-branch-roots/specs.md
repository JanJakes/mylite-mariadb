# Published Index Branch Roots

## Problem

MyLite now has a validated branch-page format, but published rebuilt indexes
still expose multi-page leaf runs directly through the first leaf page. Exact
lookups can binary-search leaf pages, but the storage root does not yet model a
navigable internal page. The next bounded step is to publish a single-level
branch root for rebuilt fixed-width leaf runs when the leaf set fits in one
branch page.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `mylite_storage_rebuild_index_leaf()` and
  `mylite_storage_rebuild_index_leaves()` collect live index entrysets, encode
  maintained roots for small fixed-width indexes, otherwise encode contiguous
  leaf pages and publish a catalog index-root record pointing at the first
  page.
- `read_index_leaf_run_root()` is the common reader setup for full, exact, and
  prefix leaf-run reads.
- Exact leaf-run lookup currently finds a candidate page by binary-searching
  leaf page key ranges.

## Scope

- For rebuilt fixed-width indexes whose leaf-page count is greater than one and
  fits in one branch page, write one branch root followed by contiguous leaf
  pages.
- Keep catalog index-root records pointing at the branch root while preserving
  the entry count as the rebuilt static snapshot count.
- Decode branch roots in `read_index_leaf_run_root()`, validate level,
  table/index ownership, key width, contiguous child pages, expected child
  count, and the first child leaf page.
- Use the branch root for exact-key child selection before reading the candidate
  leaf page.
- Leave prefix and full reads on the existing leaf-run traversal after branch
  root setup resolves the contiguous child leaf run.

## Non-Goals

- No multi-level branch trees.
- No split, merge, rebalance, or in-place branch maintenance for row DML.
- No branch roots for indexes that exceed one branch page's child capacity.
- No variable-width, collation-aware, FULLTEXT, SPATIAL, vector, or expression
  index support.

## Compatibility Impact

No SQL syntax, public API, or storage-engine routing behavior changes. Existing
supported `ENGINE=InnoDB`, omitted/default-engine, and explicit MyLite tables
continue to route to MyLite storage. This slice only changes the internal root
page used by rebuilt fixed-width indexes.

## Single-File And Lifecycle Impact

Branch roots and child leaves are durable pages inside the primary `.mylite`
file. No new companion files are introduced. Append-tail overlays continue to
start after the published static leaf run, so row DML after rebuild remains
visible through the existing append-history overlay.

## File-Format Impact

Uses existing `TABLE_INDEX_BRANCH` page type `13`, page-local version `1`.
There is still no global format-version bump because production support is
limited to newly rebuilt indexes on this branch and all reads validate the page
type, level, child count, and child layout before trusting it.

## Test Plan

- Update multi-page leaf-run storage coverage to expect a branch root and a
  first child leaf page.
- Keep exact lookups across first, second, last, and missing keys passing.
- Keep duplicate-boundary exact reads, prefix reads, full index reads, append
  tail overlays, and stale catalog-count behavior passing.
- Run:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure
```

## Acceptance Criteria

- Multi-page rebuilt fixed-width indexes publish a branch root when one branch
  page can address all child leaves.
- Branch roots read back into the existing leaf-run abstraction without
  exposing stale or non-contiguous children.
- Exact-key lookup uses branch child selection for branch-rooted leaf runs.
- Prefix, full, append-tail, and routed storage-engine tests keep passing.
- Roadmap wording still marks split/merge and maintained branch DML as pending.

## Risks And Follow-Ups

- The branch root is single-level. Larger trees need a multi-level branch
  design with fanout, split, merge, and recovery tests.
- Branch roots are published by rebuild only. Row-DML maintenance still updates
  maintained single-page roots or append-tail overlays.
- Prefix lower-bound selection is covered by the follow-up
  `branch-assisted-prefix-lookup` slice. Multi-level branch navigation remains
  pending.

## Implementation Notes

`prepare_index_leaf_rebuild_page()` now chooses a branch-rooted leaf run when
the rebuilt fixed-width index needs more than one leaf page and the leaf count
fits in one branch page. The root page stores child leaf ids and high
`(key, row_id)` fences; the leaf pages remain contiguous immediately after the
root. `read_index_leaf_run_root()` decodes either maintained roots, branch
roots, or legacy leaf roots into the same leaf-run abstraction. Exact lookup
uses the branch root to choose the candidate child leaf for branch-rooted runs.

## Verification Results

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
