# Branch Refold Child-Count Deletes

## Problem

Single-level branch roots can remove a one-entry child when a delete reduces
the expected child count. They still fall back to row-state append-tail overlay
when the same child-count reduction is caused by deleting from a multi-entry
child. In that shape, the branch can be refolded into one fewer existing child
leaf and the old final child page can be reclaimed.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB InnoDB B-tree delete maintenance is broader than this slice:
  `mariadb/storage/innobase/include/btr0btr.h` declares `btr_compress()` for
  sibling merge/lift behavior, and
  `mariadb/storage/innobase/btr/btr0btr.cc` implements sibling merge after
  locating parent and sibling pages. MyLite should keep the first-party
  single-file storage layer narrower and journal-bounded.
- MyLite branch delete planning lives in
  `packages/mylite-storage/src/storage.c`:
  `plan_branch_index_root_delete()` currently handles stable-child deletes and
  one-entry child removals, but it skips multi-entry leaves when
  `expected_child_count + 1 == child_count`.
- Existing refold code can materialize branch entries, sort fixed-width raw
  keys, and encode leaf/branch pages. Existing child removal code can reclaim a
  removed leaf page through the durable free-list path.

## Scope

- Plan a physical refold delete for a single-level branch root when:
  - the root is level `1`;
  - deleting the row reduces the expected child count by one;
  - the source child leaf contains the deleted row and has more than one entry;
  - the resulting entry count does not fit a maintained single-page root;
  - the root and all current child leaves fit the bounded protected-page
    journal plan;
  - reclaiming the old final child page fits the current free-list protection
    rules.
- Materialize all current branch child entries, remove the deleted row, and
  encode the remaining entries into the first `child_count - 1` existing child
  page ids in branch order.
- Rewrite the branch root with one fewer child cell and refreshed high-key
  fences.
- Reclaim the old final child leaf page through the durable free-list path.
- Preserve fallback behavior for unsupported shapes.

## Non-Goals

- No arbitrary-depth B-tree delete balancing.
- No multi-level branch writer maintenance.
- No maintained-root collapse for this multi-entry source shape; if the
  post-delete entryset fits a single-page root, this slice leaves the existing
  fallback path in place until a collapse-specific slice covers it.
- No arbitrary free-list chain coalescing beyond the existing reclaim path.
- No SQL-visible behavior, public API, storage-engine routing, or page-format
  change.

## Compatibility Impact

No MariaDB-visible SQL behavior changes. Eligible deletes avoid append-tail
index visibility work while preserving exact, prefix, indexed-row, and
full-index read results.

## Single-File And Lifecycle Impact

All durable state remains inside the primary `.mylite` file. The statement or
transaction journal protects the branch root, all current child leaves, and the
current free-list root when reclaim coalescing mutates it. Rollback and stale
journal recovery restore the previous branch layout and header/free-list state.

## File-Format Impact

No file-format change. The slice rewrites existing branch, leaf, journal, and
free-list page formats.

## Test Plan

- Add a storage unit test that builds a three-child single-level branch root,
  deletes from a multi-entry middle child, and verifies the root refolds into
  two full child leaves with the old final child reclaimed.
- Verify exact lookup, prefix lookup, prefix-exists, indexed-row materialization,
  and full index reads after the refold.
- Verify no append-tail index-entry fallback is needed by checking expected
  page-count growth and branch child count.
- Cover statement rollback, stale statement recovery, and stale transaction
  recovery for the refold path.
- Preserve existing branch same-child delete, child removal, collapse,
  cross-child update, split, root promotion, and storage-smoke coverage.
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

- Eligible child-count-reducing deletes from multi-entry child leaves refold the
  branch into one fewer child leaf instead of using append-tail index-entry
  fallback.
- Branch exact lookup, prefix lookup, prefix-exists, indexed-row materialization,
  and full-index reads remain correct after refold.
- Rollback and stale recovery restore the previous branch root, child leaves,
  free-list root, and logical visibility.
- Unsupported refold shapes keep the existing fallback behavior.
- Docs distinguish this bounded refold path from full B-tree merge,
  redistribution, multi-level branch writer maintenance, and arbitrary free-list
  coalescing.

## Verification Results

Verified on 2026-05-23:

```sh
git diff --check
git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c
cmake --build --preset dev --target mylite_storage_test
ctest --test-dir build/dev -R mylite-storage --output-on-failure
cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test
ctest --preset storage-smoke-dev --output-on-failure
```

## Risks And Follow-Ups

- This is a bounded single-level branch maintenance step, not a production
  arbitrary-depth B-tree delete algorithm.
- Future work still needs maintained-root collapse for this shape, multi-level
  writer maintenance, branch-page merge/redistribution policy, arbitrary-chain
  free-list coalescing, and unbounded branch cursors.
