# Leaf Range Fence Carry Forward

## Problem

After zeroed leaf-range encoding, prepared-insert output shows branch
leaf-range redistribution still encodes `24,796` replacement leaf pages and
then refreshes the parent branch from those encoded page bytes. The encoder
already knows the last ordered entry assigned to each replacement leaf, but
`refresh_index_branch_children_after_leaf_range_redistribution()` re-reads the
max cell from each encoded page to rebuild the branch child fences.

This is redundant writer-side work adjacent to the hot branch redistribution
path. The safety boundary is that branch refresh must still validate the
resulting branch ordering and entry count, and leaf checksums must still be
computed at the same publication point.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice changes only
  first-party MyLite storage code and test-hook observability.
- `prepare_zeroed_index_leaf_range_pages()` walks the ordered replacement
  entryset page by page, so it can identify each encoded leaf's max
  `(key, row_id)` from the same ordered entry used to write the leaf cells.
- `refresh_index_branch_children_after_leaf_range_redistribution()` currently
  locates each affected branch child, calls `read_encoded_index_leaf_max_cell()`
  on the encoded page, writes the branch cell fence, then validates the updated
  branch ordering through `validate_refreshed_index_branch_child()`.
- Existing branch snapshot encoding already has an equivalent pattern:
  `encode_zeroed_index_leaf_pages()` can fill `out_page_max_row_ids` and
  `out_page_max_keys` while encoding a contiguous leaf run.
- This slice does not touch maintained-root planning, recovery-journal
  protected-page validation, dirty-buffer publication policy, or checksum
  timing.

## Design

Carry leaf-range max fences from the zeroed range encoder into the branch
refresh:

- extend `prepare_zeroed_index_leaf_range_pages()` with optional
  `out_page_max_row_ids` and `out_page_max_keys` outputs, matching the existing
  contiguous-run encoder contract;
- allocate a bounded max-key array in `redistribute_branch_index_leaf_range_entry()`
  for the selected leaf range and pass it through the encoder and branch
  refresh;
- let `refresh_index_branch_children_after_leaf_range_redistribution()` use the
  carried fences when provided, while keeping the existing encoded-page read
  fallback for tests and defensive callers; and
- add a test-hook counter for `read_encoded_index_leaf_max_cell()` calls so the
  prepared-insert benchmark can show remaining encoded max-cell reads.

The branch refresh still validates child ordering and entry count after the
fences are written. Leaf bytes and checksums are unchanged.

## Compatibility Impact

No SQL behavior, public C API behavior, handler behavior, storage-engine
routing, metadata, file-format, checksum algorithm, or transaction behavior
changes. The branch page bytes produced by the redistribution writer are
intended to remain identical.

## Single-File And Lifecycle Impact

No durable files or transient companions are introduced. Journal protection,
rollback, commit, recovery, and close behavior stay unchanged.

## Binary-Size And Dependency Impact

No dependencies or license changes. Non-test-hook builds add only small helper
plumbing and a bounded temporary allocation already tied to the protected leaf
range. Test-hook builds add one counter and one benchmark row.

## Test And Verification Plan

- Add focused self-test coverage proving carried leaf-range fences refresh the
  branch without calling `read_encoded_index_leaf_max_cell()`.
- Keep the fallback branch refresh coverage for callers that do not pass
  carried fences.
- Print encoded leaf max-cell reads in the prepared-insert benchmark summary.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Branch leaf-range redistribution carries max fences from encoding into branch
  refresh without re-reading those encoded leaf pages.
- Branch refresh fallback behavior remains available and covered.
- Prepared-insert checksum and maintained-root decode totals remain aligned
  with prior evidence.
- Storage and storage-smoke verification pass.

## Evidence

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `315.91 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` size `33,977,826` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `331.67 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  full-page checksum calls `8`, zero-tail checksum calls `235,291`, index
  leaf page clears `772`, and encoded index leaf max-cell reads `776`.
  The same sample reported `81.837 us/op`, but unrelated high-CPU Chrome and
  sibling-worktree processes were active, so this timing is structural evidence
  only and should not be compared with the prior clean `76.760 us/op` sample.
- Final index-leaf encode attribution remains:

  | Site | Encoded leaf pages |
  | --- | ---: |
  | `prepare_index_branch_snapshot_pages_with_order` | 4 |
  | `prepare_zeroed_index_leaf_range_pages` | 24,796 |
  | `prepare_index_leaf_split_pages` | 772 |

- Maintained-root decode attribution remains `677` decodes:
  `read_index_leaf_run_root` `1` full-checksum decode,
  `plan_maintained_index_root_inserts` `674` decodes (`2` full checksum,
  `672` checksum-dirty), and `validate_recovery_journal_saved_page` `2`
  full-checksum decodes.
- Index-branch decode attribution remains `none | 0`.

## Risks

- The carried fence arrays must remain tied to the same ordered entryset and
  page-id order used for encoding. The helper contract must reject mismatched
  optional output pointers.
- The wall-clock timing effect may be small because this removes page-local
  reads and key copies, not checksum calculations.
