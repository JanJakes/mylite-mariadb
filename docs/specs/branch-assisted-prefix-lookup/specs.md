# Branch-Assisted Prefix Lookup

## Problem

Published fixed-width index leaf runs can now use a single-level branch root,
but prefix scans still find their first candidate leaf page by binary-searching
leaf pages directly. That keeps prefix lookup tied to extra leaf-page reads
even when the branch root already stores high-key fences for every child.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- `find_index_leaf_run_prefix_lower_page()` is the shared lower-bound helper
  used by static prefix-existence reads, overlay prefix-existence reads, and
  prefix entryset reads.
- Branch roots store one high `(key, row_id)` fence per contiguous child leaf
  page. For prefix lower-bound selection, the first child whose high key is not
  below the requested raw prefix is the first child that can contain a matching
  prefix.
- Existing prefix scanners already verify actual leaf contents and stop when
  they pass the requested prefix, so branch selection only changes the starting
  leaf page.

## Scope

- Use branch roots to choose the first candidate child leaf page for fixed-width
  prefix lookup.
- Keep the existing leaf-page prefix scanners as the correctness boundary for
  matching, row-id collection, skip-row handling, and stop-after-prefix behavior.
- Preserve fallback leaf-page binary search for legacy leaf roots and
  maintained single-page roots.
- Cover branch-assisted production behavior with multi-page prefix tests.

## Non-Goals

- No multi-level branch tree navigation.
- No branch-page split, merge, rebalance, or row-DML maintenance.
- No variable-width, collation-aware, nullable, FULLTEXT, SPATIAL, vector, or
  expression index prefix support beyond the existing fixed raw byte-key leaf
  run support.
- No SQL-visible behavior or public API change.

## Compatibility Impact

No SQL syntax, storage-engine routing, or public `libmylite` API behavior
changes. Supported routed `ENGINE=InnoDB`, omitted/default engine, and explicit
MyLite tables continue to expose the same exact and prefix lookup results. The
slice only changes which durable index pages are read before existing prefix
matching starts.

## Single-File And Lifecycle Impact

The branch root and leaf pages are durable pages inside the primary `.mylite`
file. No companion files are introduced. Append-tail overlays remain visible
through the existing overlay scan that starts after the immutable leaf run.

## File-Format Impact

No new page type or layout change. This slice reuses
`TABLE_INDEX_BRANCH` page type `13` with page-local version `1`.

## Test Plan

- Keep existing multi-page branch-root prefix coverage for first-page,
  second-page, missing-prefix, full-key-prefix, and append-tail-overlay probes.
- Add a branch-root production locality test that corrupts an unrelated
  non-candidate leaf page after clearing caches, then proves a prefix lookup in
  the last leaf page still succeeds through branch lower-bound selection.
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

- Branch-rooted leaf runs use branch high-key fences to choose the prefix
  lower-bound child page.
- The existing prefix scanners still validate leaf contents before returning
  matches.
- Legacy leaf roots, maintained roots, append-tail overlays, and exact lookups
  keep passing.
- Roadmap wording no longer lists branch-assisted prefix lookup as pending.

## Risks And Follow-Ups

- Single-level branch roots still only cover rebuilt fixed-width leaf runs that
  fit in one branch page.
- Prefix lookup still scans forward through matching leaf pages after the
  branch-selected lower bound. Multi-level branch trees and split/merge
  maintenance remain separate storage slices.

## Implementation Notes

`find_index_leaf_run_prefix_lower_page()` now delegates branch-rooted leaf runs
to a branch-root lower-bound helper. The helper decodes the branch page,
validates table/index/key-width/level/child-count compatibility with the leaf
run, binary-searches child high-key fences by prefix, and returns the candidate
leaf page offset. Existing page-local prefix scanners still validate and collect
matches.

## Verification Results

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `ctest --preset storage-smoke-dev --output-on-failure`
