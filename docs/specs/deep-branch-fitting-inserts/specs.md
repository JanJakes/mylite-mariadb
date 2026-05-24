# Deep Branch Fitting Inserts

## Problem

Branch readers already descend multi-level branch roots recursively, and
level-`5` roots can now be produced by bounded level-`4` root promotion. The
write side still routes maintained inserts through hard-coded level-specific
functions through level `4`, so a fitting insert into an existing level-`5` or
deeper root falls back to an append-tail index-entry page even when the selected
leaf has spare capacity.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB insert execution reaches engines through `handler::ha_write_row()` in
  `mariadb/sql/handler.cc`, which calls the virtual `handler::write_row()`
  contract declared in `mariadb/sql/handler.h`. This slice remains first-party
  MyLite storage work behind the handler boundary.
- `packages/mylite-storage/src/storage.c` has recursive branch readers that
  validate parent/child branch fences and child levels above level `4`.
- The maintained insert planner currently dispatches only level `1` through
  level `4` branch-root writers, leaving deeper roots to append-tail fallback.
- The rollback journal can protect
  `MYLITE_STORAGE_FORMAT_JOURNAL_MAX_PROTECTED_PAGES` dirty pages. A fitting
  deep insert dirties the root-to-leaf branch path plus the leaf page, so this
  slice must bound maintained depth by that journal capacity.

## Scope

- Add a generic maintained fitting-insert path for branch roots deeper than
  level `4`.
- Descend the selected branch path using the same high `(key, row_id)` fence
  selection as readers, falling back to the last child for high-key appends.
- Validate each branch child page, level, key width, table id, index number,
  nonzero entry count, and parent fence before planning.
- Plan only when the selected leaf has spare capacity.
- Protect every branch page on the selected path plus the selected leaf in the
  statement or transaction journal.
- Rewrite the selected leaf and refresh branch fences and entry counts
  bottom-up without publishing a fallback index-entry page.

## Non-Goals

- No split propagation below level-`5` roots.
- No arbitrary-depth unbounded mutation; maintained depth is bounded by rollback
  journal capacity.
- No update/delete maintenance for deeper roots.
- No file-format, SQL, public API, or storage-engine routing change.

## Compatibility Impact

SQL-visible lookup and ordering semantics remain unchanged. Eligible fixed-width
raw index inserts avoid one append-tail index-entry page when a deeper static
branch tree already has room in the selected leaf.

## Single-File And Lifecycle Impact

All durable state remains in the primary `.mylite` file. The inserted row page
is appended after the statement-start header; the existing branch path and leaf
page are journal-protected and rewritten in place. Rollback restores the
previous branch path and truncates the appended row page.

## File-Format Impact

No file-format change. Existing branch and leaf page formats already encode
arbitrary positive branch levels and page-owned entry counts.

## Test Plan

- Extend the packed-root split storage test after level-`5` promotion.
- Insert one more high key while the newly split rightmost leaf has spare
  capacity.
- Cover statement rollback and committed insert.
- Assert the fitting insert grows the file by one row page only, keeps the root
  at level `5`, preserves the level-`5` child count, and updates exact lookup,
  prefix lookup, indexed-row materialization, and full entryset reads.
- Run focused storage tests, storage-smoke tests, `git diff --check`, and
  clang-format diff checks for touched C files.

## Acceptance Criteria

- Eligible fitting inserts below level-`5` and deeper branch roots refresh the
  selected branch path without an append-tail index-entry page.
- Unsupported full-leaf, invalid-tree, too-deep, and split-propagation cases
  keep existing fallback behavior.
- Statement rollback restores the previous deeper branch tree and file size.
- Existing level-specific branch insert, split, promotion, read, and recovery
  coverage remains passing.

## Verification Results

Passed on 2026-05-24:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`

## Risks And Follow-Ups

- General recursive split propagation remains future B-tree work.
- The generic path is intentionally fitting-only; full leaves below deeper roots
  still fall back until recursive split planning exists.
- Deeper update/delete maintenance remains future work.
