# Branch Same-Child Updates

## Problem

Branch-root updates currently only rewrite the final child leaf when the
replacement `(key, row_id)` still belongs in that final child. Updates for
interior children fall back to append-tail overlay even when the replacement
entry remains inside the same leaf range and can be made durable with the same
root-plus-leaf journal protection.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB source changes
  are needed.
- MyLite single-level branch roots store one fence tuple per child leaf. A
  child owns entries greater than the previous child fence and, for non-final
  children, less than or equal to its current fence.
- Existing update journaling already protects a branch root plus one leaf
  before in-place branch updates.
- Existing branch leaf rewrite code can rebuild one leaf and refresh that
  child's branch fence.

## Scope

- Plan branch updates for any child leaf that contains the source row.
- Accept the physical update only when the replacement tuple remains in the
  same child range:
  - above the previous child fence, if one exists;
  - at or below the current child fence for non-final children;
  - unbounded above for the final child.
- Continue to reject cross-child movement and any update requiring split or
  merge.
- Keep one-root-plus-one-leaf journal protection.

## Non-Goals

- No branch split, merge, borrow, or redistribution.
- No movement between child leaves.
- No multi-level branch trees.
- No broad optimizer or SQL behavior changes.

## Compatibility Impact

No SQL, public API, or storage-engine routing behavior changes. Supported
updates avoid writing redundant append-tail index-entry pages for more branch
children.

## Single-File And Lifecycle Impact

The update remains inside the primary `.mylite` file. Statement rollback and
stale-journal recovery restore the protected branch root and leaf page.

## Test Plan

- Extend branch-root storage coverage with an interior-child update that stays
  below that child's current high fence.
- Assert the update is visible inside a statement, does not append a fallback
  index-entry page, and rolls back to the prior root and leaf state.
- Keep existing final-child update/delete/removal coverage.
- Run the storage and storage-smoke verification set.

## Acceptance Criteria

- Interior same-child updates rewrite the source leaf and branch fence without
  append-tail index entries.
- Cross-child updates remain on the existing append-tail fallback path.
- Rollback restores the prior leaf, branch root, and logical visibility.
- Docs distinguish same-child updates from broader child-boundary movement.

## Implementation Notes

- Branch update planning now scans every child leaf in the single-level branch
  root until it finds the source row.
- A shared child-range predicate accepts replacements that remain above the
  previous child fence and, for non-final children, at or below the current
  child fence.
- The write path stores the planned child index, validates the same child again
  before rewriting, rebuilds the single leaf, and refreshes that child fence.
- Storage coverage exercises an interior child update inside a statement,
  asserts no fallback index-entry page is appended, and verifies rollback
  restores the old key.

## Verification Results

- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --preset storage-smoke-dev --output-on-failure`
- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`

## Risks And Follow-Ups

- The accepted range is intentionally conservative because branch pages store
  child high fences, not all neighboring child minimums.
- Cross-child movement remains future split/merge or redistribution work.
