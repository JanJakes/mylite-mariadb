# Maintained Root Decode Checksum-State Attribution

## Problem

After recovery-journal snapshot reuse, the prepared-insert profile reports
`677` maintained-root decodes but only `5` `index-root` full-page checksum
calls. The remaining root decodes are dominated by
`plan_maintained_index_root_inserts` with `674` decodes, but the existing
maintained-root decode table does not show whether a caller paid a full
checksum scan or decoded checksum-dirty in-memory bytes.

That distinction matters before changing the protected planning path. Planning
decodes are still the safety gate for root metadata, row references, and key
ordering. The benchmark should identify whether remaining work is full-page
checksum validation or checksum-skipped parser work.

## Source Findings

- MariaDB base remains `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`). This slice changes only
  first-party MyLite test-hook observability.
- `decode_maintained_index_root_page_with_checksum_state()` already receives a
  `checksum_dirty` flag. When set, the decoder skips the full checksum scan but
  still validates page type, version, format, table/index/key metadata,
  capacity, supported flags, row references, and key order.
- The test-hook maintained-root decode site counter records caller function
  names but collapses checksum-validating and checksum-dirty decodes into one
  count.
- Prepared-insert checksum call-site output still records the shared
  `decode_maintained_index_root_page` checksum site, but not the caller that
  caused each full checksum.

## Design

Extend the test-hook maintained-root decode site table with two bounded counts
per caller:

- full checksum decodes (`checksum_dirty == 0`);
- checksum-dirty decodes (`checksum_dirty != 0`).

Keep the aggregate per-site decode count so existing benchmark readers can
compare current output with older profiles. Update the prepared-insert
benchmark table to print:

`Site | Decodes | Full checksum | Checksum dirty`

The slice does not change page validation, checksum calculation, storage
mutation, dirty-buffer publication, journal behavior, or durable bytes.

## Compatibility Impact

No SQL behavior, public C API behavior, handler API behavior, storage-engine
routing, metadata, file-format, checksum algorithm, or write policy changes.
The slice only changes test-hook counters and benchmark output.

## Single-File And Lifecycle Impact

No durable files or transient companion files are introduced. Statement,
journal, rollback, commit, and close/recovery lifecycles are unchanged.

## Binary-Size And Dependency Impact

No dependencies or license changes. Non-test-hook builds are unchanged.
Test-hook builds add two small bounded counter arrays parallel to the existing
maintained-root decode site table.

## Test And Verification Plan

- Extend the maintained-root decode site counter self-test to assert aggregate,
  full-checksum, checksum-dirty, and reset behavior.
- Update `tools/mylite_perf_baseline.c` to print the new columns.
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

- The prepared-insert maintained-root decode table shows per-site aggregate,
  full-checksum, and checksum-dirty counts.
- Existing aggregate maintained-root decode counts remain unchanged.
- The output identifies which caller owns the remaining `index-root` full-page
  checksum calls.
- Storage and storage-smoke verification pass.

## Evidence

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `295.42 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` size `33,977,714` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `370.69 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  prepared-insert step `80.365 us/op`, full-page checksum calls `8`,
  zero-tail checksum calls `235,291`.
- Final maintained-root decode attribution:

  | Site | Decodes | Full checksum | Checksum dirty |
  | --- | ---: | ---: | ---: |
  | `read_index_leaf_run_root` | 1 | 1 | 0 |
  | `plan_maintained_index_root_inserts` | 674 | 2 | 672 |
  | `validate_recovery_journal_saved_page` | 2 | 2 | 0 |

- Final index-branch decode attribution remains `none | 0`.

## Risks

- The table is test-hook observability, not a stable public API. Future caller
  names may change with refactors.
- If the site table limit is reached, aggregate checksum counters remain the
  fallback signal and the table can be expanded in a separate evidence slice.
