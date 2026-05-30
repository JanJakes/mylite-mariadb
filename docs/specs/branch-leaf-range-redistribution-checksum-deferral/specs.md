# Branch Leaf-Range Redistribution Checksum Deferral

## Problem

After maintained-root checksum work, the prepared-insert benchmark still reports
`242,827` zero-tail checksum calls. The largest remaining writer-side branch
checksum site is:

- `7,537` `index-branch` zero-tail calls under
  `refresh_index_branch_children_after_leaf_range_redistribution`.

Those calls happen after the redistribution writer has already decoded the
branch page, rebuilt the affected leaf range, and validated the updated branch
child fences. The branch page is then written through the maintained-index dirty
buffer, which can carry checksum-dirty pages and refresh them at publication.

## Source Findings

- This is first-party MyLite storage work in
  `packages/mylite-storage/src/storage.c` plus storage self-test coverage.
- `redistribute_branch_index_leaf_range_entry()` reads the branch through
  `read_branch_insert_writer_branch_page()`, validates table/index/key/level
  metadata, rebuilds the leaf range, calls
  `refresh_index_branch_children_after_leaf_range_redistribution()`, then writes
  the branch through `pager_write_maintained_insert_page()`.
- `refresh_index_branch_children_after_leaf_range_redistribution()` validates
  that every updated child remains in branch order with
  `validate_refreshed_index_branch_child()` before it updates the branch entry
  count.
- `pager_write_maintained_insert_page()` can store existing maintained index
  pages in the dirty-page buffer with `checksum_dirty=1`. Direct fallback writes
  and dirty-buffer flushes refresh the checksum before durable publication.
- `redistribute_level_two_branch_index_leaf_range_entry()` calls the same lower
  redistribution helper, then immediately reads the updated child branch and
  passes it to `refresh_index_branch_child_after_branch_insert()`, which still
  performs a checksum-validating `decode_index_branch_page()` on the child.

## Design

Add an internal refresh switch to
`refresh_index_branch_children_after_leaf_range_redistribution()` and thread it
through `redistribute_branch_index_leaf_range_entry()`.

The top-level single-level branch redistribution writer uses no-refresh mode,
zeros the branch checksum slot, and writes the page with `checksum_dirty=1`.
That preserves the current branch metadata and child-fence validation while
letting the dirty-buffer publication path refresh the checksum once.

The level-two caller keeps the lower child-branch redistribution on
refresh-checksum mode in this slice, because the root-refresh helper still
performs an immediate checksum-validating child-branch decode. Avoiding that
decode safely is a separate slice.

Planning and recovery-journal validation are unchanged. Durable reads still use
the checksum-validating branch decoder.

## Compatibility Impact

No SQL behavior, public API behavior, storage-engine routing, metadata layout,
file-format, checksum algorithm, or durable page image changes. Durable branch
pages are still published with valid checksums.

## Single-File And Lifecycle Impact

No files are introduced. Statement rollback, journal protection, dirty-page
undo capture, dirty-buffer pressure flushing, commit flushing, close, and
recovery stay on the existing paths.

## Binary Size And Dependency Impact

No new dependency. The change is a small internal writer switch and focused
self-test coverage.

## Tests And Verification Plan

- Extend branch-child refresh self-test coverage to prove branch leaf-range
  redistribution can skip the immediate zero-tail checksum, leave the checksum
  slot stale, and later validate after the existing dirty checksum refresh
  helper runs.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Benchmark Evidence

After this slice, the prepared-insert benchmark no longer reports
`refresh_index_branch_children_after_leaf_range_redistribution` as an
`index-branch` zero-tail checksum call site. The deferred branch page is
refreshed by dirty-buffer publication instead.

Current `prepared-insert-components 1000 100000` counters:

- prepared insert step: `92.130 us/op`;
- full-page checksum calls: `2,329`;
- zero-tail checksum calls: `235,291`;
- `index-branch` zero-tail checksum calls: `390`;
- `index-branch` dirty checksum refreshes: `2`;
- `index-branch` dirty-page flush refreshes: `2`;
- maintained-root decodes remain `1,449`.

The remaining `index-branch` zero-tail call sites are:

- `386` under `encode_index_branch_page`;
- `2` under `encode_index_branch_page_from_leaf_run`;
- `2` under `refresh_dirty_buffered_page_checksum`.

## Verification Results

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed in `373.70 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed; archive size `33,979,034` bytes.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed in `338.95 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed.

## Acceptance Criteria

- `refresh_index_branch_children_after_leaf_range_redistribution` no longer
  appears as an `index-branch` zero-tail call site for the top-level
  prepared-insert path.
- Prepared-insert correctness and storage-smoke tests remain green.
- Dirty-buffer publication still reports checksum refreshes for any deferred
  branch pages.

## Risks

- A branch page can carry a stale checksum while resident in the dirty buffer.
  The writer still validates branch metadata, child ids, child fences, entry
  count capacity, and key order before buffering the page, and publication
  refreshes the checksum before durable write.
- The nested level-two path intentionally remains conservative in this slice.
  Removing its immediate child-branch decode requires a separate validation
  change to `refresh_index_branch_child_after_branch_insert()`.
