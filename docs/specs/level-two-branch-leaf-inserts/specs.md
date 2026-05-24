# Level Two Branch Leaf Inserts

## Problem

MyLite can split a packed single-level branch root into a bounded level-`2`
branch tree, and readers can navigate that tree. The next insert into a
non-full leaf under one of the lower level-`1` branches still falls back to an
append-tail index-entry page because insert planning only maintains
single-level branch roots.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc` and `mariadb/sql/handler.h` keep index mutation and
  cursor behavior behind the handler API; this slice does not need upstream
  SQL-layer changes.
- `mariadb/storage/mylite/ha_mylite.cc` passes supported fixed-width raw key
  images into first-party MyLite storage row-DML helpers.
- `packages/mylite-storage/src/storage.c` already supports:
  - single-level branch leaf insert maintenance;
  - full single-level branch root splitting into a level-`2` root;
  - level-`2` exact, prefix, prefix-exists, and full-entryset reads.
- `plan_branch_index_root_insert()` currently returns without a maintained plan
  when `branch_page->level != 1U`, so level-`2` row inserts publish a row page
  plus fallback index-entry tail instead of updating the relevant lower branch
  and leaf pages.

## Scope

- Plan and write a maintained insert when:
  - the catalog index root is a level-`2` branch page;
  - the selected child is a valid level-`1` branch page;
  - the selected leaf belongs to that child branch and has free capacity;
  - the key size and table/index identifiers match throughout the path.
- Rewrite the selected leaf, the lower branch page, and the root branch page.
- Update entry counts and high-key fences at both branch levels.
- Protect all three dirty pages in the statement or transaction journal.
- Preserve append-tail fallback for full leaves, deeper roots, unsupported
  shapes, and broad split/merge cases.

## Non-Goals

- No level-`2` leaf split.
- No insertion into roots deeper than level `2`.
- No multi-level update/delete maintenance.
- No branch merge, borrow, redistribution, or compaction.
- No SQL-visible behavior, public API, storage-engine routing, or file-format
  change.

## Compatibility Impact

SQL results and handler-visible ordering remain unchanged. The slice only
removes one internal append-tail fallback for supported fixed-width routed
storage indexes after a root split has already published a level-`2` branch
tree.

## Single-File And Lifecycle Impact

All durable state stays in the primary `.mylite` file. The selected leaf,
lower branch page, and root branch page are protected by the existing journal
before mutation. The inserted row page is appended and becomes durable only
when the header page count is published. Rollback restores the previous branch
pages and truncates the appended row page.

## File-Format Impact

No file-format change. Existing branch page level, child page id, entry count,
and high-key fence fields are updated.

## Test Plan

- Extend storage branch-root coverage so a committed packed-root split is
  followed by a maintained insert into the new level-`2` branch tree.
- Assert the maintained insert grows the file by one row page and does not
  write a fallback index-entry page.
- Verify exact lookup, prefix lookup, indexed-row materialization, and full
  index reads after the maintained insert.
- Verify statement rollback restores the level-`2` root and lower branch state.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible level-`2` branch inserts rewrite the selected leaf, lower branch,
  and root branch pages instead of appending fallback index-entry pages.
- Branch entry counts and high-key fences stay valid at both branch levels.
- Rollback restores the previous branch tree and file size.
- Existing single-level branch insert, split, update, delete, read, and
  recovery coverage remains passing.

## Verification Results

Passed on 2026-05-23:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- Full leaves under level-`2` roots still fall back until a later lower-branch
  split slice.
- Multi-level update/delete maintenance, branch merge/redistribution, and
  broad B-tree balancing remain future work.
