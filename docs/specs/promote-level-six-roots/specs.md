# Promote Level Six Roots

## Problem

Deep branch roots can now promote to level `6`, and full level-`5` child
branches under a level-`6` parent can split while that parent has child
capacity. Once the level-`6` root itself is child-cell-full, the same
no-overlay high-key insert still falls back to append-tail index-entry pages.
Promoting that full level-`6` root to a bounded level-`7` root is the next
incremental split-propagation step.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB reaches this storage work through `handler::ha_write_row()` calling
  `write_row()` in `mariadb/sql/handler.cc`, with the non-virtual handler API
  declared in `mariadb/sql/handler.h`. This remains first-party MyLite storage
  work below the MariaDB handler boundary.
- `packages/mylite-storage/src/storage.c` already prepares the full lower split
  chain through level `5` and can journal-protect the original root, selected
  branch path, and full leaf within the maintained-index journal bound.
- Promoting a level-`6` root needs sibling level-`5` entry counts, just as
  level-`5` root promotion needs sibling level-`4` entry counts, because prior
  splits can leave sibling subtree entry counts non-uniform.

## Scope

- Extend the generic deep branch insert plan to cover full selected leaves when:
  - the root is exactly level `6`;
  - the selected leaf is full;
  - the selected level-`1` branch is packed and full;
  - the selected level-`2`, level-`3`, level-`4`, level-`5`, and root level-`6`
    branches are child-cell-full;
  - the static branch subtree has no live append-tail overlay for the
    table/index; and
  - the promoted level remains within the maintained branch level limit.
- Append one split leaf page, one new branch page at each level `1` through
  `4`, one new right level-`5` branch page, and two new level-`6` branch pages.
- Rewrite the original leaf, selected level-`1` through level-`5` branches, and
  the root page as a level-`7` root with two level-`6` children.

## Non-Goals

- No split of child-cell-full level-`6` branches under existing level-`7`
  parents.
- No root promotion above level `7`.
- No update/delete maintenance for deeper roots.
- No file-format, SQL, public API, storage-engine routing, wire-protocol,
  binary-size, license, or dependency change.

## Compatibility Impact

SQL-visible ordering and lookup results remain unchanged. Eligible fixed-width
raw index inserts avoid an append-tail index-entry fallback when a full
level-`6` root can promote to a bounded level-`7` root.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. The inserted row page,
split leaf page, new level-`1` through level-`4` branch pages, one new
level-`5` branch page, and two new level-`6` branch pages are appended after the
statement-start header. Existing root/path pages and the original full leaf are
journal-protected. Rollback restores the previous level-`6` root and truncates
the appended pages.

## File-Format Impact

No file-format change. Existing leaf and branch page encodings are reused.

## Test Plan

- Extend the packed-root split storage test after level-`6` level-five branch
  split coverage by filling the level-`6` root and final level-`5`, level-`4`,
  level-`3`, level-`2`, level-`1`, and leaf path.
- Add a statement-rolled-back high-key insert that promotes the full level-`6`
  root to level `7`.
- Commit the same insert and assert the file grows by one row page, one split
  leaf page, one new branch page at each level `1` through `4`, one new
  level-`5` branch page, and two level-`6` branch pages.
- Verify root level, root child count, left/right level-`6` child counts,
  rollback, exact lookup, prefix lookup, indexed-row materialization, and full
  entryset reads.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible child-cell-full level-`6` roots promote to level `7` without an
  append-tail index-entry page.
- Root-full shapes above level `6` and broader recursive split-propagation
  cases keep existing fallback behavior.
- Statement rollback restores the previous level-`6` root and file size.
- Existing fitting-insert, leaf split, lower-branch split, child-branch split,
  upper-branch split, level-four branch split, level-five branch split,
  level-`6` root promotion, read, and recovery coverage remains passing.

## Verification Results

- `clang-format -i packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure` passed
  in `1010.35` seconds.
- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  passed.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
  passed.
- `ctest --preset storage-smoke-dev --output-on-failure` passed `10/10`
  tests in `1045.53` seconds.

## Risks And Follow-Ups

- Splitting level-`6` branches under existing level-`7` and deeper parents
  remains future recursive B-tree work.
- Deeper update/delete maintenance remains future work.
- The public-write regression that fills a level-`6` root now dominates the
  storage test runtime, so deeper split work should first add cheaper generated
  shape coverage or a recursive split harness.
