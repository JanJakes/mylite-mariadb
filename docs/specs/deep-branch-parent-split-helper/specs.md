# Deep Branch Parent Split Helper

## Problem Statement

The maintained raw-index writer now supports bounded split propagation through
level-`8` branches. The implementation in
`split_deep_branch_level_four_entry()` repeats the same parent-list rewrite
shape for level-`7`, level-`8`, and level-`9` parents: copy existing child
cells, replace the selected child with the left and right split children,
preserve fences, and optionally read sibling child entry counts before splitting
the parent itself.

That duplication increases the risk of subtle divergence before adding a
future level-`9` root-promotion or recursive propagation slice.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB parser, optimizer, and handler behavior do not change. This slice is
  limited to first-party MyLite storage code after MariaDB has routed the row
  write to the MyLite handler.
- `packages/mylite-storage/src/storage.c` owns the relevant implementation:
  `plan_deep_branch_index_root_insert()` chooses bounded split/promotion paths,
  and `split_deep_branch_level_four_entry()` publishes the selected leaf,
  level-`1` through level-`8` branch pages, and refreshed ancestors.
- The level-`7` and level-`8` parent paths need sibling entry counts when the
  parent itself is about to split or promote. The level-`9` parent rewrite for
  level-`8` branch splits only needs copied page ids and fences.

## Scope

- Extract a local helper for expanding a branch parent child list by replacing
  one selected child with left/right split-child cells.
- Let the helper optionally read, validate, and return entry counts for
  unchanged sibling branch children when the caller will split that parent.
- Use the helper for the existing level-`7`, level-`8`, and level-`9` parent
  rewrite paths inside `split_deep_branch_level_four_entry()`.

## Non-Goals

- No new split eligibility, level-`9` root promotion, or recursive propagation.
- No page-layout, file-format, catalog, public API, storage-engine routing, or
  wire-protocol change.
- No test-fixture behavior change beyond proving the refactor preserves the
  existing coverage.

## Design

Add a storage-local helper that receives the decoded parent branch, selected
child offset, replacement left/right child cells, expected sibling child level,
and output arrays for page ids, optional entry counts, max row ids, and max
keys.

For the selected child, the helper writes two output cells: the left split child
and the appended right split child. For all other children, it copies the page
id, max row id, and max key from the parent payload. When sibling entry counts
are requested, it reads each unchanged sibling page, decodes it as a branch page
under the same header, validates table id, index number, key size, expected
child level, non-zero entry count, non-`ULLONG_MAX` entry count, and the parent
fence, then writes that sibling entry count into the output array.

Callers keep allocating level-specific arrays, computing split counts,
encoding pages, and choosing which dirty pages to publish. That keeps the
refactor small and avoids changing the existing page-write order.

## Compatibility Impact

No SQL, C API, storage-engine routing, or application-visible compatibility
change. The helper preserves the existing append, rollback, and commit
behavior for supported maintained raw-index split paths.

## DDL Metadata Routing Impact

None.

## Single-File And Recovery Impact

No new durable files or companion files. The same protected pages are journaled,
the same pages are appended or rewritten, and the same rollback/recovery
coverage remains authoritative.

## Public API, File Format, And Routing Impact

No public C API, file-format version, branch-page encoding, catalog-root, or
handler-routing change.

## Build, Size, And Dependencies

No new dependencies or generated sources. The change may add one small static
helper while reducing repeated code in the hot split writer.

## Test Plan

- Run the direct storage unit binary.
- Run `ctest --test-dir build/dev -R mylite-storage --output-on-failure`.
- Run `git diff --check` and `git clang-format --diff`.
- Run the storage-smoke build and `ctest --preset storage-smoke-dev
  --output-on-failure`.

Existing fixture-backed tests cover level-`7` branch split, level-`8` root
promotion, level-`8` branch split, and full level-`9` parent fallback.

## Acceptance Criteria

- Existing maintained deep-branch split, root-promotion, fallback, rollback,
  and commit tests remain passing.
- The helper validates sibling branch pages before using sibling entry counts.
- The diff does not change storage format, public API, or SQL-visible behavior.

## Risks And Open Questions

- This is still fixed-depth propagation. A future slice should use the helper
  either as a step toward recursive split propagation or remove it when a more
  general recursive writer replaces the current bounded path.
