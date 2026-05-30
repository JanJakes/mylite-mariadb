# Index Branch Decode Site Counters

## Problem

The prepared-insert checksum profile still reports `386` full-page
`index-branch` checksum calls under the aggregate `decode_index_branch_page`
call site. Unlike maintained-root decodes, branch decodes are not yet split by
caller, so it is unclear whether the remaining work is journal validation,
planning, or writer-side redundancy.

The prior branch leaf-range leaf checksum-deferral experiment was rejected
because it reduced `encode_zeroed_index_leaf_page` calls but increased
dirty-buffer checksum refreshes and slowed prepared insert. The next safe
performance slice needs better attribution before changing branch decode or
checksum behavior.

## Source Findings

- `decode_index_branch_page()` validates branch magic, page id, owner fields,
  checksum, child count, entry count, child page ids, and child fence ordering.
- The checksum call-site table records this work only as
  `decode_index_branch_page`, because the checksum is computed inside the
  decoder.
- Maintained-root decodes already use test-hook caller-site attribution through
  `decode_maintained_index_root_page_at_site()` and benchmark accessors.
- Prepared-insert writer counters already show zero branch insert writer
  decodes, but the branch checksum table cannot independently prove where the
  remaining full-page branch checksums come from.

## Design

Add test-hook-only caller-site attribution for `decode_index_branch_page()` that
matches the maintained-root decode-site pattern:

- wrap branch decoder calls with a `decode_index_branch_page_at_site()` macro in
  test-hook builds;
- record the caller function name only when checksum profiling is enabled;
- expose slot-count, slot-name, and count accessors for the benchmark; and
- print a prepared-insert branch decode-site table next to the maintained-root
  decode-site table.

No storage behavior, checksum validation, or durable page format changes.

## Compatibility Impact

No SQL, public API, file-format, storage-engine routing, or durability behavior
changes. The new accessors are test-hook/profiling evidence only.

## Single-File And Lifecycle Impact

No file lifecycle changes. Recovery journal validation and protected-page
validation remain checksum-validating gates.

## Binary Size And Dependency Impact

No dependency changes. Non-test-hook builds keep the existing branch decoder
signature and do not allocate the new counters.

## Tests And Verification Plan

- Add storage self-test coverage proving branch decode-site counters record the
  immediate caller and reset with prepared-insert profile counters.
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

- Prepared-insert benchmark output includes branch decode call-site counts.
- Existing checksum and maintained-root decode counters remain intact.
- Storage and storage-smoke verification pass.

## Implementation Evidence

Implemented as a test-hook-only wrapper around `decode_index_branch_page()`,
with benchmark accessors and a focused storage self-test that verifies caller
attribution, reset behavior, and out-of-range accessor handling. Non-test-hook
builds keep the original branch decoder signature.

Verification passed:

- `git diff --check`
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
- `cmake --build --preset dev --target mylite_storage_test`
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

The storage-smoke static archive measured `33,979,034` bytes (`32.40 MiB`).
The prepared-insert component benchmark reported `2,329` full-page checksum
calls, `235,291` zero-tail checksum calls, `1,449` maintained-root decodes, and
the new branch decode-site table attributed all `386` index-branch decodes to
`split_branch_index_leaf_entry`. That makes the remaining branch full-page
checksum work a branch-split writer hot spot rather than recovery-journal
validation.

## Risks

The slice intentionally adds evidence rather than removing checksum work. It
should stay test-hook-only so it does not affect release storage behavior or
binary profile.
