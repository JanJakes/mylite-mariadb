# Dirty Branch Page Checksum Refresh

## Problem

Prepared inserts into already-published branch roots repeatedly rewrite the same
routing branch page inside the active statement. The dirty-page buffer already
coalesces those durable writes, but the writer still refreshes the branch page
checksum before every buffered rewrite. Local sampling of:

```sh
./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components --profile-iterations=200000 1000
```

showed `checksum_page_zero_tail()` below branch leaf insertion and branch child
refresh. A byte-loop micro-optimization was measured and rejected because it
regressed the focused benchmark, so this slice moves the checksum work to the
existing dirty-buffer boundary instead.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- This slice changes first-party MyLite storage code only:
  `packages/mylite-storage/src/storage.c`.
- `insert_branch_index_leaf_entry()` refreshes the selected child fence and
  then writes the existing branch root through
  `pager_write_maintained_insert_page(..., buffer_existing_page = 1)`.
- `buffer_dirty_page_for_pager_write()` already buffers maintained root and
  branch pages when an active statement can coalesce the rewrite.
- Active branch-page caches read trusted metadata with
  `read_index_branch_page_cache_metadata()` and do not need a valid checksum
  while the page remains statement-owned.
- Durable reads and flushes still require a valid FNV-1a page checksum.

## Design

Add a dirty checksum bit to dirty-page buffer entries. Maintained branch/root
writers may store a checksum-zero page in the active dirty buffer and mark the
entry dirty. The checksum is refreshed:

- when a dirty buffered page is copied for a generic pager read; and
- immediately before the dirty buffer is flushed to the primary `.mylite` file.

The fast fitting single-level branch insert path skips the branch-page checksum
refresh and writes that page through the dirty checksum path. Leaf pages keep
their existing eager checksum because they are not buffered by the maintained
root/branch dirty-page buffer.

If the dirty-page path cannot buffer a dirty-checksum page, the pager refreshes
the checksum on a local copy before writing directly.

## Non-Goals

- No checksum algorithm change.
- No page-format change.
- No durable validation weakening.
- No change to append-page checksum-dirty handling.
- No broader branch split, redistribution, refold, update, or delete change.

## Compatibility Impact

No SQL, C API, handler, storage routing, metadata, or wire-protocol behavior
changes. Durable pages still contain the same checksum bytes at checkpoint and
read boundaries.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. The dirty checksum bit is
statement-local process memory and is cleared with the existing dirty-page
buffer lifecycle.

## Build, Size, And Dependencies

Small first-party C change only. No new dependency or embedded build-profile
change.

## Test Plan

- Add a storage unit regression that dirty-buffered branch pages refresh their
  checksum when copied and when flushed.
- Run the storage unit suite.
- Run storage-smoke embedded storage-engine coverage.
- Run the prepared insert component benchmark before and after the change.
- Run `git diff --check` and `git clang-format --diff` on touched C files.

## Acceptance Criteria

- Dirty buffered branch/root pages are never persisted with a zero or stale
  checksum.
- Generic reads of dirty buffered branch/root pages receive checksum-valid
  bytes.
- Existing storage and routed storage-engine tests pass.
- The focused prepared insert component benchmark does not regress relative to
  the same-machine `HEAD` comparison.

## Verification Results

- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  passed.
- `cmake --build --preset dev --target mylite_storage_test` passed.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  passed: 1/1 test, 157.86 seconds.
- `ctest --test-dir build/storage-smoke-dev -R libmylite.embedded-storage-engine --output-on-failure`
  passed: 1/1 test, 46.13 seconds.
- Same-machine `HEAD` comparison before this slice measured
  `prepared insert step component` at `27.097`, `27.387`, and `27.677 us/op`.
- After this slice, three sequential runs of
  `./build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 10000`
  measured `prepared insert step component` at `22.814`, `25.217`, and
  `22.831 us/op`. The benchmark emitted the known CSV-engine fallback message.

## Risks And Open Questions

- This only removes redundant checksums while a maintained branch/root page is
  dirty-buffered. Wider SQLite-like insert performance still needs broader
  pager and B-tree maintenance work.
