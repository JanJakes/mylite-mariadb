# Leaf Split Fence Carry Forward

## Problem

Prepared-insert attribution now shows `776` encoded index leaf max-cell reads.
The residual table attributes `772` of them to
`copy_index_branch_children_with_split()`, which rebuilds branch child arrays
after a leaf split by reading the max cell from the two freshly encoded split
leaf pages.

The split encoder already walks the sorted entryset and therefore knows the max
`(row_id, key)` fence for each replacement leaf. Reading those same cells back
from encoded page bytes is redundant writer-side work. The safety boundary is
that durable leaf and branch page validation, branch child ordering checks,
maintained-root planning, and recovery-journal validation must remain intact.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice changes only
  first-party MyLite storage code and docs.
- `prepare_index_leaf_split_pages()` builds a raw-entry order, encodes the
  first and second replacement leaves from that order, and frees the order.
- `copy_index_branch_children_with_split()` copies unchanged branch children
  from a decoded branch page, then calls `read_encoded_index_leaf_max_cell()`
  once for each replacement split leaf.
- Call-site attribution after the prior observability slice reports:

  | Site | Reads |
  | --- | ---: |
  | `copy_index_branch_children_with_split` | 772 |
  | `encode_index_branch_page_from_leaf_run` | 4 |

- Branch split writers either validate the rebuilt branch inputs with
  `validate_index_branch_encode_components()` or decode/validate the freshly
  encoded branch page in less-hot paths. Durable reads and recovery-journal
  protected-page validation stay separate.

## Design

Carry split-leaf max fences from the split encoder into branch child copying:

- extend `prepare_index_leaf_split_pages()` with optional
  `out_leaf_max_row_ids[2]` and contiguous `out_leaf_max_keys` outputs;
- compute those fences from the same ordered entry indexes used to encode the
  two replacement leaves;
- extend `copy_index_branch_children_with_split()` with optional carried split
  fences, while keeping the existing encoded-page read fallback for defensive
  callers and focused tests;
- thread the carried fences through all branch leaf-split writer paths that use
  the split helper; and
- add focused test-hook coverage proving fallback still records two encoded
  max-cell reads and carried fences record zero.

The resulting branch bytes should be unchanged because the carried fences are
derived from the same ordered entries that were just written into the split
leaf pages.

## Compatibility Impact

No SQL behavior, public C API behavior, handler behavior, storage-engine
routing, metadata, file format, checksum algorithm, or transaction behavior
changes. This affects only writer-local branch array construction after
validated leaf split inputs are available.

## Single-File And Lifecycle Impact

No durable files or transient companions are introduced. Journal protection,
rollback, recovery, commit, and close behavior stay unchanged.

## Binary-Size And Dependency Impact

No dependencies or license changes. The helper signatures grow by two optional
fence outputs/inputs, and split writers carry a small row-id pair plus a bounded
two-key buffer while constructing the replacement branch child arrays.

## Test And Verification Plan

- Extend focused storage self-test coverage so
  `copy_index_branch_children_with_split()` fallback records two max-cell reads
  and carried split fences preserve branch child fences with zero max-cell
  reads.
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

- Prepared-insert encoded leaf max-cell read attribution no longer reports
  `copy_index_branch_children_with_split()`.
- Aggregate encoded leaf max-cell reads drop by the prior `772` reads while the
  remaining `4` direct branch snapshot reads stay visible.
- Maintained-root planning/recovery-journal validation counts do not drop as a
  side effect.
- Storage and storage-smoke verification pass.

## Evidence

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `373.06 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` size `33,979,834` bytes (`32.41 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `328.76 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  full-page checksum calls `8`, zero-tail checksum calls `235,291`, index
  leaf page clears `772`, and encoded index leaf max-cell reads `4`. The same
  sample reported `81.924 us/op`, but unrelated high-CPU Chrome and
  `/projects/mylite` jobs were active, so the timing is structural evidence
  only.
- Encoded leaf max-cell read attribution now reports only:

  | Site | Reads |
  | --- | ---: |
  | `encode_index_branch_page_from_leaf_run` | 4 |

- `copy_index_branch_children_with_split` no longer appears in the encoded
  leaf max-cell read call-site table.
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

- The optional carried-fence buffers must use the same key size and entry order
  as the encoded split leaf pages. Keep the fallback path to isolate this
  helper contract from unrelated callers.
- The performance effect may be smaller than the structural counter drop
  because this removes page-local reads and key copies, not checksum calls.
