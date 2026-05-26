# Branch Range Refresh Targeted Validation

## Problem

After single-child branch refresh stopped fully decoding the just-mutated
branch page, prepared insert sampling still shows full branch decode work in
`refresh_index_branch_children_after_leaf_range_redistribution()`.

That function updates the max key and row id for a bounded set of existing leaf
children after redistribution, recomputes the durable branch checksum, and then
calls `decode_index_branch_page()` on the already-decoded branch bytes.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice is first-party
  MyLite storage code only.
- Leaf range redistribution starts from a decoded and validated level-one
  branch page.
- The refresh helper changes only existing child max fences and increments the
  branch entry count by one.
- Child page ids, page metadata, level, child count, and used-byte shape are
  unchanged by the refresh.

## Design

Reuse the targeted child-fence validation introduced for single-child refresh:

- Update each matched child fence from the redistributed leaf pages.
- Validate the incremented branch entry count against branch capacity.
- Validate branch child ordering after all range updates.
- Keep the durable checksum write with `checksum_page_zero_tail()`.

Do not change leaf redistribution planning or leaf page encoding in this slice.

## Compatibility Impact

No SQL-visible or public API behavior changes. Valid redistribution writes the
same branch bytes, and invalid child ordering still returns
`MYLITE_STORAGE_CORRUPT`.

## Single-File And Lifecycle Impact

No file-format or companion-file changes. This only narrows validation after a
trusted in-memory branch range mutation.

## Binary-Size And Dependency Impact

No dependency changes. The slice reuses the existing targeted validator and
extends the storage hook test.

## Test And Verification Plan

- Extend the branch refresh hook so range refresh succeeds without
  `checksum_page()` calls from a full decode.
- Assert an out-of-order redistributed range is rejected.
- Keep storage and storage-smoke tests passing.
- Run `git diff --check`, `git clang-format --diff`, and a prepared insert
  component benchmark.

## Acceptance Criteria

- `refresh_index_branch_children_after_leaf_range_redistribution()` no longer
  calls `decode_index_branch_page()` after mutating child fences.
- Updated range child fences remain checked for ordering.
- Branch entry-count overflow or capacity violations still fail.
- Relevant tests, formatting checks, and the prepared insert component
  benchmark pass.

## Risks

- The helper validates the changed branch ordering and entry count, not every
  unchanged child address. That matches the trusted mutation scope because the
  branch page was decoded before redistribution and child page ids are not
  modified here.
