# Branch Snapshot Fence Carry Forward

## Problem

After leaf-range and leaf-split fence carry-forward, the prepared-insert
benchmark reports only `4` encoded index leaf max-cell reads. Attribution shows
all four under `encode_index_branch_page_from_leaf_run()`, the internal helper
that builds a direct branch snapshot from a freshly encoded contiguous leaf run.

That helper validates each just-encoded leaf header, then reads the max cell
from the encoded leaf bytes. The leaf-run encoder already knows each page's max
`(row_id, key)` while assigning entries to leaf pages, so the readback is
redundant writer-side work.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice changes only
  first-party MyLite storage code and docs.
- `prepare_index_branch_snapshot_pages_with_order()` allocates a branch page
  followed by a contiguous encoded leaf run, then calls
  `encode_zeroed_index_leaf_pages()` and
  `encode_index_branch_page_from_leaf_run()`.
- `encode_zeroed_index_leaf_pages()` already accepts optional
  `out_page_max_row_ids` and `out_page_max_keys` arrays and fills them from the
  same ordered entries used to encode each leaf page.
- `encode_index_branch_page_from_leaf_run()` currently validates the leaf page
  id, table id, index number, key size, and nonzero entry count before calling
  `read_encoded_index_leaf_max_cell()`.
- Prepared-insert attribution after leaf split fence carry-forward reports
  only:

  | Site | Reads |
  | --- | ---: |
  | `encode_index_branch_page_from_leaf_run` | 4 |

## Design

Carry branch snapshot leaf fences from `encode_zeroed_index_leaf_pages()` into
`encode_index_branch_page_from_leaf_run()`:

- allocate bounded statement-local max-row and max-key buffers in
  `prepare_index_branch_snapshot_pages_with_order()`;
- ask `encode_zeroed_index_leaf_pages()` to populate those buffers while
  encoding the contiguous leaf run;
- extend `encode_index_branch_page_from_leaf_run()` with optional carried fence
  inputs;
- keep its existing leaf header validation before writing each branch child
  cell; and
- keep the encoded-page read fallback for defensive callers and tests.

The branch snapshot page bytes should remain unchanged because the carried
fences come from the same ordered entries that were just written into the leaf
pages.

## Compatibility Impact

No SQL behavior, public C API behavior, handler behavior, storage-engine
routing, metadata, file format, checksum algorithm, or transaction behavior
changes.

## Single-File And Lifecycle Impact

No durable files or transient companions are introduced. Journal protection,
rollback, recovery, commit, and close behavior stay unchanged.

## Binary-Size And Dependency Impact

No dependencies or license changes. The branch snapshot preparation path gains
bounded stack buffers whose total max-key payload is guarded by page-size and
branch-child capacity.

## Test And Verification Plan

- Extend branch snapshot layout self-test coverage to assert snapshot
  preparation records zero encoded leaf max-cell reads.
- Keep existing branch snapshot child id and max-fence assertions.
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

- Prepared-insert encoded leaf max-cell reads drop from `4` to `0`.
- The encoded leaf max-cell read call-site table reports `none | 0`.
- Branch snapshot layout coverage still verifies child ids and max fences.
- Maintained-root planning/recovery-journal validation counts remain unchanged.
- Storage and storage-smoke verification pass.

## Evidence

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `424.37 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` size `33,980,162` bytes (`32.41 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `332.32 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  full-page checksum calls `8`, zero-tail checksum calls `235,291`, index
  leaf page clears `772`, and encoded index leaf max-cell reads `0`. The same
  sample reported `75.420 us/op`, but unrelated high-CPU Chrome and
  `/projects/mylite` jobs were active, so the timing is structural evidence
  only.
- Encoded leaf max-cell read attribution now reports:

  | Site | Reads |
  | --- | ---: |
  | none | 0 |

- Index-leaf encode attribution remains:

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

- The carried max-key buffer must stay sized to `leaf_page_count * key_size`.
  The implementation must guard that product even though branch capacity should
  already bound it.
- This removes the last measured max-cell readback, but checksum generation and
  dirty-buffer publication remain the dominant prepared-insert storage work.
