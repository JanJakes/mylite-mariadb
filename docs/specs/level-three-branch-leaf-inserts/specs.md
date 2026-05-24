# Level Three Branch Leaf Inserts

## Problem

After a child-cell-full level-`2` branch root promotes to a level-`3` root,
readers can navigate the deeper static tree. The next insert into a non-full
leaf under that level-`3` tree still falls back to append-tail index-entry pages
because insert maintenance only rewrites through level-`2` roots.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB index mutation remains behind handler calls in
  `mariadb/sql/handler.h` and `mariadb/sql/handler.cc`; this slice stays in
  first-party MyLite storage.
- `packages/mylite-storage/src/storage.c` already maintains fitting inserts
  through level-`2` roots by rewriting the selected leaf, lower branch, and
  root.
- The existing branch refresh helpers can update one branch from a rewritten
  leaf and update a parent branch from a rewritten child branch.

## Scope

- For a level-`3` root whose selected path is:
  - a valid level-`2` child branch;
  - a valid level-`1` lower branch;
  - a selected leaf with spare capacity;
  rewrite the leaf, lower level-`1` branch, level-`2` child branch, and level-`3`
  root directly.
- Update entry counts and high-key fences at all three branch levels.
- Protect all four dirty existing pages in the statement or transaction
  journal.
- Preserve append-tail fallback for full leaves, deeper roots, invalid fences,
  and broad split/merge cases.

## Non-Goals

- No level-`3` leaf split.
- No lower-branch, middle-branch, or root split under a level-`3` root.
- No update/delete maintenance for level-`3` roots.
- No arbitrary-depth recursive insert maintenance.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible row order and lookup results remain unchanged. The storage layer
only removes one append-tail fallback for supported fixed-width raw indexes
after a level-`3` root has already been published.

## Single-File And Lifecycle Impact

All state remains in the primary `.mylite` file. The leaf, level-`1` branch,
level-`2` branch, and root branch pages are journal-protected before mutation.
The inserted row page is appended and becomes durable when the header page
count is published. Rollback restores the previous branch pages and truncates
the appended row page.

## File-Format Impact

No file-format change. Existing branch page level, child page id, entry count,
and high-key fence fields are updated.

## Test Plan

- Extend the packed-root split storage test after level-`3` promotion with a
  statement-rolled-back fitting insert into the promoted level-`3` tree.
- Commit the same insert and assert the file grows by one row page only, not by
  a fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, full
  entryset reads, root entry count, root level, statement rollback, and final
  committed visibility.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible fitting inserts under level-`3` roots rewrite all branch ancestors
  without an append-tail index-entry page.
- Full-leaf and deeper-root cases keep existing fallback behavior.
- Rollback restores the previous level-`3` tree and file size.
- Existing level-`2` and level-`3` promotion coverage remains passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Level-`3` split propagation remains future work.
- General arbitrary-depth insert, update, delete, balancing, and compaction
  remain future B-tree work.
