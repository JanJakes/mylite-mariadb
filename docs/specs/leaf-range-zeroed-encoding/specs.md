# Leaf Range Zeroed Encoding

## Problem

Prepared-insert checksum attribution now shows the remaining maintained-root
full-checksum work is limited to durable validation gates, while index-leaf
encoding still accounts for `25,572` zero-tail checksum calls. The latest
storage-smoke sample attributes `24,796` of those encoded leaf pages to
`prepare_index_leaf_range_pages()`.

That path is branch leaf-range redistribution. It allocates a fresh replacement
page run, then calls the generic leaf encoder that clears each full page before
writing metadata, cells, and the zero-tail checksum. The fresh allocation can
provide zeroed pages up front, avoiding the redundant per-page clear without
moving checksum calculation or weakening writer validation.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice changes only
  first-party MyLite storage code and test-hook observability.
- `redistribute_branch_index_leaf_range_entry()` reads and validates the
  level-one branch and selected child leaf pages, builds a sorted replacement
  entryset, allocates `updated_leaf_pages`, encodes the replacement leaves, then
  writes each leaf through the existing pager path and refreshes the branch
  child fences.
- `prepare_index_leaf_range_pages()` accepts caller-owned page memory and uses
  `encode_index_leaf_page()`, which clears the full page before delegating to
  `encode_zeroed_index_leaf_page()`. Its current self-test intentionally passes
  stack memory filled with `0xa5`, so the generic helper must keep supporting
  non-zeroed buffers.
- `prepare_index_branch_snapshot_pages_with_order()` already uses `calloc()`
  plus `encode_zeroed_index_leaf_pages()` for freshly allocated leaf runs,
  proving the storage layer already has a zeroed-buffer encoding pattern.
- The previous rejected leaf checksum-deferral experiments changed checksum
  timing and dirty-buffer refresh behavior. This slice does not defer leaf
  checksums or change dirty-page admission, flush, recovery, or validation.

## Design

Add a zeroed-buffer variant for leaf-range encoding and use it only when
`redistribute_branch_index_leaf_range_entry()` allocates fresh replacement
leaves:

- allocate `updated_leaf_pages` with `calloc()` after the existing overflow
  check;
- encode each replacement page with `encode_zeroed_index_leaf_page()` against
  the explicit `leaf_page_ids[]` array, preserving non-contiguous child page
  support;
- keep the existing generic `prepare_index_leaf_range_pages()` for stack or
  reused buffers that need full-page clearing;
- preserve index-leaf encode-site attribution for the zeroed range helper; and
- expose a test-hook leaf-page-clear counter in the prepared-insert benchmark.

The checksum call-site table should remain comparable: leaf pages still compute
their zero-tail checksum during encoding, and durable or dirty-buffered pages
still carry the same bytes as before.

## Compatibility Impact

No SQL behavior, public C API behavior, handler behavior, storage-engine
routing, metadata, file-format, checksum algorithm, or transaction behavior
changes. The encoded leaf page bytes are intended to remain identical.

## Single-File And Lifecycle Impact

No durable files or transient companions are introduced. Journal protection,
rollback, commit, recovery, and close behavior stay unchanged.

## Binary-Size And Dependency Impact

No dependencies or license changes. Non-test-hook builds add one small static
helper; test-hook builds add one clear-count accessor and one benchmark row.

## Test And Verification Plan

- Add focused storage self-test coverage proving the zeroed range helper
  preserves range layout, records the encode site, and avoids
  `encode_index_leaf_page()` full-page clears.
- Keep the existing generic range helper test proving non-zeroed caller buffers
  are still cleared and ordered correctly.
- Print `index leaf page clears` in the prepared-insert benchmark summary.
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

- Branch leaf-range redistribution uses the zeroed-buffer encoder only for its
  freshly allocated replacement leaf run.
- Prepared-insert checksum totals and durable validation gates remain aligned
  with prior evidence; this slice should not remove maintained-root planning or
  recovery-journal validation decodes.
- Prepared-insert benchmark output reports index-leaf page clears, and the
  range-redistribution clears are removed from that count.
- Storage and storage-smoke verification pass.

## Evidence

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `415.30 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` size `33,977,586` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `321.70 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared-insert step `76.760 us/op`, full-page checksum calls `8`,
  zero-tail checksum calls `235,291`, and index leaf page clears `772`.
- Final index-leaf encode attribution:

  | Site | Encoded leaf pages |
  | --- | ---: |
  | `prepare_index_branch_snapshot_pages_with_order` | 4 |
  | `prepare_zeroed_index_leaf_range_pages` | 24,796 |
  | `prepare_index_leaf_split_pages` | 772 |

- The aggregate checksum call-site row remains
  `encode_zeroed_index_leaf_page | index-leaf | 0 | 25,572`.
- Maintained-root decode attribution remains `677` decodes:
  `read_index_leaf_run_root` `1` full-checksum decode,
  `plan_maintained_index_root_inserts` `674` decodes (`2` full checksum,
  `672` checksum-dirty), and `validate_recovery_journal_saved_page` `2`
  full-checksum decodes.
- Index-branch decode attribution remains `none | 0`.

## Risks

- `calloc()` may not improve wall-clock time on every allocator or host even
  though it removes the redundant encoder clear. The benchmark result must be
  recorded rather than assumed.
- The zeroed helper is safe only for freshly zeroed buffers. Reused or stack
  page buffers must keep using the generic clearing helper.
