# Bulk Branch Snapshot Leaf Writes

## Problem

Branch-root refolds encode a complete branch snapshot as one branch page followed
by a contiguous run of freshly encoded leaf pages. The current publisher writes
those leaf pages one page at a time through `pager_write_prevalidated_index_leaf_page()`.
Fresh sampling after dirty branch-page checksum deferral still shows
`write_index_branch_snapshot_pages()` spending time below per-leaf
`write_page_at_raw()` calls.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice changes first-party MyLite storage code only:
  `packages/mylite-storage/src/storage.c`.
- `prepare_index_branch_snapshot_pages()` already returns the branch page plus
  a contiguous leaf page run in one memory buffer.
- `write_index_branch_snapshot_pages()` writes each leaf separately even though
  callers pass append-position leaf page ids.
- The append-page buffer is shared with row appends, so branch snapshot leaves
  should preserve the old direct-write lifetime rather than occupying that
  buffer inside large insert statements.
- Active leaf-page cache refresh only needs metadata from the trusted encoded
  pages, not checksum-validating decodes.

## Design

Add a prevalidated leaf-run pager helper used by
`write_index_branch_snapshot_pages()`. When the leaf run starts at or after the
current header page count, publish it with one contiguous direct file write,
then refresh active leaf-page cache metadata from the encoded pages. If a
caller ever passes an existing-page range, fall back to the existing per-page
prevalidated writer so dirty-page undo behavior remains unchanged.

The branch root remains on the maintained root/branch dirty-page writer.

## Non-Goals

- No branch snapshot layout change.
- No page-format or checksum algorithm change.
- No change to branch root dirty buffering.
- No attempt to batch arbitrary non-contiguous leaf rewrites.

## Compatibility Impact

No SQL, C API, handler, storage routing, metadata, or wire-protocol behavior
changes. Durable leaf pages are byte-identical; only publication batching
changes.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. During an active
statement, newly appended snapshot leaves preserve the previous direct-write
lifetime while reducing write-call overhead for the contiguous leaf run.

## Build, Size, And Dependencies

Small first-party C change only. No new dependency or embedded build-profile
change.

## Test Plan

- Extend branch snapshot publication coverage to assert appended snapshot
  leaves do not occupy the row append-page buffer while the existing branch root
  is staged in the dirty-page buffer.
- Run the storage unit suite.
- Run storage-smoke embedded storage-engine coverage.
- Run the prepared insert component benchmark.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

## Acceptance Criteria

- Branch snapshot leaves publish through one contiguous direct write for append
  ranges.
- Existing-page leaf writes still use the old per-page path.
- Existing storage and routed storage-engine tests pass.
- The focused prepared insert component benchmark does not regress locally.

## Verification Results

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed: 1/1 test, 156.27 seconds.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  passed.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  passed: 1/1 test, 36.71 seconds.
- Two sequential runs of
  `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  measured `prepared insert step component` at `23.620` and `22.860 us/op`.
- A long local run,
  `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components --profile-iterations=200000 1000`,
  measured `prepared insert step component` at `33.916 us/op`. A pre-slice
  profiler run in the same session measured `35.157 us/op`, with a noisier
  sample at `41.130 us/op`. The benchmark emitted the known CSV-engine fallback
  message.

## Risks And Open Questions

- This reduces per-page write overhead during refolds, but branch refold still
  rebuilds and checksums snapshot leaves. Broader SQLite-like performance still
  needs less frequent full refolds and deeper B-tree/pager work.
