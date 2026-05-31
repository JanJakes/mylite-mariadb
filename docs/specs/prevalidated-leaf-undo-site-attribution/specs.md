# Prevalidated Leaf Undo Site Attribution

## Problem

Branch leaf-range redistribution and branch split writers now use
`pager_write_prevalidated_index_leaf_page()` for replacement leaves. The writer
still captures dirty-page undo preimages, but unlike the generic
`pager_write_page()` test-hook wrapper it did not set the undo-capture write
site name. The prepared-insert profile therefore kept the aggregate
dirty-page-undo copy count but hid most caller-level attribution needed for the
next undo/checksum slice.

## Source Findings

- MariaDB base line: MariaDB 11.8.6,
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- The affected code is first-party MyLite storage test-hook observability in
  `packages/mylite-storage/src/storage.c`; no upstream MariaDB SQL or handler
  code is involved.
- `pager_write_page()` is wrapped in test-hook builds so
  `capture_dirty_page_undo_for_pager_write()` can attribute dirty-buffer
  preimage copies to the calling storage writer.
- `pager_write_prevalidated_index_leaf_page()` uses the same undo-capture
  helper and durable write path, but publishes only active index-leaf cache
  metadata after the write.
- The prevalidated writer needs the same test-hook site wrapper so call-site
  output remains comparable after replacing generic leaf writes.

## Design

Add a test-hook-only
`pager_write_prevalidated_index_leaf_page_at_site()` wrapper and macro, matching
the generic pager write attribution pattern. The wrapper sets
`test_dirty_page_copy_undo_write_site_name` around undo capture and restores the
previous value on all exits.

Extend the existing undo-site self-test so it writes one dirty-buffered leaf
through `pager_write_page()` and one dirty-buffered leaf through
`pager_write_prevalidated_index_leaf_page()`, then asserts both undo-capture
copies are attributed to the caller.

## Compatibility Impact

No SQL-visible behavior, public C API behavior, handler API behavior,
storage-engine routing, file format, checksum algorithm, or durable write
policy changes. Non-test-hook builds keep the same prevalidated writer
implementation.

## Single-File And Lifecycle Impact

No durable file, journal, recovery, lock, sidecar, or embedded lifecycle
change. Dirty-page undo capture and recovery protection are unchanged.

## Test And Verification Plan

- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `build/dev/packages/mylite-storage/mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- The focused storage self-test proves generic and prevalidated leaf writes both
  attribute dirty undo-capture copies by caller.
- Prepared-insert aggregate checksum, dirty-copy, maintained-root decode, and
  dirty publication counters do not change.
- The undo-capture write-site table again exposes prevalidated leaf writer
  callers instead of hiding those copies under the aggregate total.

## Risks

- This is observability only. It should not be counted as a durable storage
  behavior or checksum-count optimization.

## Verification Results

- `git diff --check`: clean.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c`:
  clean.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `build/dev/packages/mylite-storage/mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `312.79 sec`.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `324.17 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed with `33,984,626` bytes and `478` archive members.

The prepared-insert benchmark
(`/tmp/mylite-prevalidated-leaf-undo-site-attribution-benchmark.txt`) reported:

- prepared insert step: `74.965 us/op`;
- full-page checksum calls: `8`;
- zero-tail checksum calls: `227,063`;
- dirty `index-leaf` refreshes: `87,176`;
- maintained-root decodes: `677`;
- dirty undo-capture copies: `664`, all `index-leaf`;
- undo-capture write sites: `654` dirty leaf copies from
  `redistribute_branch_index_leaf_range_entry` and `10` from
  `split_branch_index_leaf_entry`.
