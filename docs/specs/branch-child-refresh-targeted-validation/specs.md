# Branch Child Refresh Targeted Validation

## Problem

Prepared insert sampling after active writer cache reuse still shows hot
checksum validation in `refresh_index_branch_child_after_leaf_insert()`:

- The function updates one child cell's max key and row id after a leaf insert.
- It recomputes the branch checksum for the durable page write.
- It then calls `decode_index_branch_page()` on the same in-memory page, which
  recomputes the checksum and revalidates every child cell.

The full decode is redundant for this trusted mutation path. The branch page
was already decoded before the mutation, and only one child fence plus the
entry count changed.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code only.
- `insert_branch_index_leaf_entry()` decodes the branch page before calling
  `refresh_index_branch_child_after_leaf_insert()`.
- `refresh_index_branch_child_after_leaf_insert()` mutates only the matched
  child cell's max row/key and increments the branch entry count.
- `decode_index_branch_page()` validates checksum, metadata shape, child page
  addresses, and global child ordering. For this path, unchanged metadata and
  unchanged child page ids were already validated.

## Design

Replace the post-mutation full branch decode in
`refresh_index_branch_child_after_leaf_insert()` with targeted validation:

- Remember the child index for the updated cell.
- Validate the incremented branch entry count against the branch capacity.
- Validate the updated child max remains strictly ordered after the previous
  child and before the next child using the same key/row-id ordering rule as
  `decode_index_branch_page()`.
- Keep writing the durable branch checksum with `checksum_page_zero_tail()`.

Do not change other branch refresh helpers in this slice.

## Compatibility Impact

No SQL-visible or public API behavior changes. The durable branch page bytes
remain the same for valid inserts, and corrupt local ordering still returns
`MYLITE_STORAGE_CORRUPT`.

## Single-File And Lifecycle Impact

No file-format or companion-file changes. The optimization only changes
transient validation after a trusted in-memory branch mutation.

## Binary-Size And Dependency Impact

No dependency changes. The slice adds a small targeted validation helper and a
storage test hook.

## Test And Verification Plan

- Add a storage test hook that exercises branch child refresh without any
  `checksum_page()` calls from full branch decode.
- In the same hook, mutate a child max beyond its next sibling and assert the
  targeted ordering validation rejects it.
- Keep storage and storage-smoke tests passing.
- Run `git diff --check`, `git clang-format --diff`, and a prepared insert
  component benchmark.

## Acceptance Criteria

- `refresh_index_branch_child_after_leaf_insert()` no longer calls
  `decode_index_branch_page()` after the single-child mutation.
- The changed child cell remains checked against neighboring child fences.
- Branch entry-count overflow or capacity violations still fail.
- Relevant tests, formatting checks, and the prepared insert component
  benchmark pass.

## Risks

- This intentionally validates only the mutation surface, not every unchanged
  child address. That is acceptable because the branch page was already decoded
  before the mutation path and the child addresses are not modified here.
