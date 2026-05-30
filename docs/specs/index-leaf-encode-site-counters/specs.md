# Index Leaf Encode Site Counters

## Problem

After maintained-root writer-side decode work, the prepared-insert profile is
dominated by checksum work outside the protected root path. The latest
storage-smoke sample reports `235,291` zero-tail checksum calls, with
`113,741` in the `index-leaf` family. Of those, `25,572` are attributed only to
the shared `encode_zeroed_index_leaf_page` helper.

That aggregate helper attribution is too coarse for a safe follow-up behavior
slice. Previous leaf checksum-deferral experiments reduced encoder checksum
calls but increased dirty-buffer refreshes and regressed prepared insert, so
the next change needs caller-level evidence before moving checksum work across
publication boundaries.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice changes only
  first-party MyLite storage test-hook observability.
- `encode_zeroed_index_leaf_page()` writes page metadata, leaf cells, and a
  zero-tail checksum for one already-zeroed page.
- `encode_zeroed_index_leaf_pages()` is the multi-page helper used by
  `prepare_index_leaf_pages()` and branch snapshot preparation.
- `encode_index_leaf_page()` is the clearing wrapper used by direct leaf split,
  update, delete, and test helpers that pass stack or reused buffers.
- The checksum call-site table records zero-tail checksum work inside
  `encode_zeroed_index_leaf_page`, but it does not identify the higher-level
  caller that requested each encoded leaf page.

## Design

Add test-hook-only index-leaf encode site counters:

- record leaf pages encoded through direct `encode_index_leaf_page()` callers;
- record leaf pages encoded through multi-page `encode_zeroed_index_leaf_pages()`
  callers using the higher-level caller site, not the shared helper name;
- expose slot-count, slot-name, and count accessors; and
- print a prepared-insert index-leaf encode-site table in
  `mylite_perf_baseline`.

The slice keeps the existing checksum call-site table unchanged, so older
aggregate profiles still compare directly with current output.

## Compatibility Impact

No SQL behavior, public C API behavior, handler behavior, storage-engine
routing, metadata, file-format, checksum algorithm, or durability behavior
changes. The counters are test-hook observability only.

## Single-File And Lifecycle Impact

No durable files, transient companions, journal behavior, rollback behavior,
commit behavior, or close/recovery behavior changes.

## Binary-Size And Dependency Impact

No dependencies or license changes. Non-test-hook builds are unchanged.
Test-hook builds add one bounded caller-site table parallel to existing
benchmark counters.

## Test And Verification Plan

- Add a focused storage self-test proving direct and multi-page leaf encode
  callers are counted separately and reset by prepared-insert profile reset.
- Update `tools/mylite_perf_baseline.c` to print the new table.
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

- Prepared-insert benchmark output shows index-leaf encode-site counts.
- The new table accounts for the leaf pages encoded by the shared leaf
  encoding helpers without changing the aggregate checksum counters.
- Existing storage and storage-smoke verification pass.

## Evidence

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `309.73 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` size `33,977,714` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `338.71 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared-insert step `77.144 us/op`, full-page checksum calls `8`,
  zero-tail checksum calls `235,291`.
- Final index-leaf encode attribution:

  | Site | Encoded leaf pages |
  | --- | ---: |
  | `prepare_index_branch_snapshot_pages_with_order` | 4 |
  | `prepare_index_leaf_range_pages` | 24,796 |
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

- This is attribution, not a performance optimization by itself.
- Caller names are test-hook evidence and may change with future refactors.
