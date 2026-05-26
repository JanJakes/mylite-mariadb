# Branch Refold Planner Sorted Entryset

## Problem Statement

Branch-refold cache maintenance now preserves cached entrysets in sorted
`(key, row_id)` order. The insert planner still copied that sorted cache and
appended the current row before deciding whether the branch root could be
refolded. For cyclic secondary-key insert workloads, that same-insert refold
entryset can become unsorted even though the preserved cache is sorted, forcing
`prepare_index_leaf_pages()` to allocate and build a raw-entry order array.

This slice makes the branch-refold planning path insert the current logical
entry into the copied entryset in sorted order as well.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- The affected path is first-party MyLite storage code in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB file changes are
  involved.
- `build_branch_index_refold_insert_entryset_if_fit()` copies an active
  branch-refold entryset cache when available, then appends the row being
  inserted before handing the entryset to `prepare_index_branch_snapshot_pages()`.
- `prepare_index_leaf_pages()` can skip raw-order allocation when the entryset
  is already sorted. Preserving sort order in the planner keeps the refold
  snapshot on that direct path.
- `tools/mylite_perf_baseline.c` prepared-insert component workloads use
  cyclic secondary-key values, so the inserted key commonly belongs before
  existing cached entries.

## Design

- Reuse the existing sorted raw-entry insertion helper for the planner's
  copied branch-refold entryset.
- Keep the general append helper unchanged because many callers intentionally
  gather unsorted data and sort during leaf preparation.
- Add an internal test-hook regression that seeds the active refold cache,
  calls the real refold planner with a middle-key insert, and verifies the
  planned entryset is sorted and does not allocate a raw-order array.

## Compatibility Impact

No SQL, public C API, storage-engine routing, metadata, or wire-protocol
behavior changes. The resulting branch leaf pages contain the same logical
entries in the same sorted order that refold publication already requires.

## Single-File And Lifecycle Impact

No file-format, durable storage, companion-file, locking, recovery, or embedded
lifecycle changes. The affected entryset is transient statement-local planning
state.

## Binary Size And Dependencies

No dependency changes. The code-size impact is one call-site change plus a test
extension.

## Test And Verification Plan

- Extend the branch-refold cache test hook to exercise
  `build_branch_index_refold_insert_entryset_if_fit()` with a middle-key insert.
- Verify the planned row-id/key order and that
  `build_raw_index_entry_order_if_needed()` does not allocate an order array.
- Run the storage test target and CTest selection.
- Run formatting checks, storage-smoke embedded storage-engine coverage, and
  the prepared-insert component performance baseline.

## Acceptance Criteria

- Branch-refold planning preserves sorted order for copied cached entrysets.
- The regression fails against append-only planner behavior and passes with
  sorted insertion.
- Existing cache preservation and invalidation behavior remains unchanged.
- Targeted tests and checks pass.

## Risks

- This relies on branch-refold caches being sorted before the planner copies
  them. The previous cache-maintenance slice added that invariant and this slice
  keeps it true for the current insert being planned.
