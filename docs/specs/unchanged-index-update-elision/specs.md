# Unchanged Index Update Elision

## Problem

The routed update path still appends replacement index-entry pages for every
index even when an update changes only non-key columns. The local update
benchmark exercises that common case with primary-key updates of a payload
column, so each durable update still writes a row page, row-state page, and a
redundant replacement index page.

The active update rewrite slice removes repeated replacement chains only while
the current replacement run remains in the active append buffer. Once a row is
updated from a durable checkpoint, the first replacement still writes all index
entries. Reducing that write volume is the next safe step before a WAL-backed
pager or maintained B-tree pages exist.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc:2623-2770` builds replacement index
  entries from `new_data`, performs duplicate-key and foreign-key checks, and
  calls `mylite_storage_update_row_with_index_entries()`.
- `mariadb/storage/mylite/ha_mylite.cc:3943-3997` uses MariaDB `key_copy()` to
  build fixed key images from a row buffer. The handler has both `old_data` and
  `new_data`, so it can compare old and new key images before calling storage.
- `packages/mylite-storage/src/storage.c:4526-4678` appends a replacement row,
  row-state page, and all supplied index-entry pages for every update.
- `packages/mylite-storage/src/storage.c:10577-10848` has the contiguous inline
  update writer, active buffered rewrite path, and index-entry page writer that
  determine physical update page count.
- `packages/mylite-storage/src/storage.c:11961-12086` and
  `packages/mylite-storage/src/storage.c:15785-15864` build exact-index and
  live-index views by hiding source row ids after row-state replacement pages.
  Those views must be taught to inherit unchanged index entries when no later
  replacement entry supersedes them.

## Design

- Add a MyLite storage update entry point that receives the full replacement
  index-entry set plus a parallel changed-entry byte vector.
- Preserve `mylite_storage_update_row_with_index_entries()` as the existing
  "all entries changed" API for current callers and tests.
- Write only changed index-entry pages in the flagged update path. The row page
  and row-state page are still written for every update, because the storage
  layer needs a replacement row id and rollback-visible source-row mapping.
- Keep active exact-index cache maintenance based on the full replacement
  entry set. Cache correctness should not depend on which index pages were
  physically written.
- Update live-index and exact-index scan overlays:
  - a replacement row-state page remaps already-seen matching source row ids to
    the replacement row id;
  - a later replacement index-entry page for the same row and index supersedes
    the inherited entry, removing the inherited row id when the key changed and
    replacing it when the key remained equal;
  - delete row-state pages still remove matching source row ids.
- Update the handler `update_row()` path to compare old and new key images with
  MariaDB's own key serialization, then call the flagged storage entry point
  for durable MyLite rows. Runtime-volatile MEMORY/HEAP rows keep their
  existing in-memory path.

## Affected Subsystems

- MyLite durable storage update publication.
- Active update buffered rewrite validation.
- Durable exact-index, leaf-tail overlay, live-index, and exact-index cache
  loading.
- MariaDB MyLite handler update path.

## Compatibility Impact

SQL semantics do not change. Supported routed `ENGINE=InnoDB`, omitted/default,
MyISAM, Aria, and explicit MyLite tables still expose the same update,
duplicate-key, index lookup, and foreign-key behavior. The change is an
internal physical-write optimization.

## Single-File And Lifecycle Impact

No new companion file, file-format version, or public durable page type is
introduced. Replacement row-state pages continue to provide rollback and
visibility rules inside one primary `.mylite` file.

## Public API And File-Format Impact

The first-party storage C API gains a flagged update variant. Existing storage
APIs and the page format remain compatible. Files written before this slice
still read correctly because the scan overlay handles both full replacement
index pages and omitted unchanged index pages.

## Binary-Size And Dependency Impact

First-party storage C and MyLite handler changes only. No new dependency.

## Tests And Verification

- Add storage coverage for an update that marks all indexes unchanged and
  verifies:
  - the file grows by only two pages;
  - primary and secondary exact lookups inherit the replacement row id;
  - full live-index reads expose the replacement row id; and
  - the old row id is hidden.
- Add storage coverage for a mixed update that keeps one index unchanged and
  changes another, including published leaf-root overlay lookup.
- Run storage unit tests, embedded storage-engine smoke, the full
  storage-smoke CTest suite, update performance baseline, `git diff --check`,
  and `git clang-format --diff`.

## Verification Results

- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  rebuilt the MariaDB embedded storage-smoke archive with the handler changes.
- `cmake --build --preset storage-smoke-dev --target mylite_storage_test mylite_embedded_storage_engine_test mylite_perf_baseline`
  rebuilt the first-party storage, embedded smoke, and performance targets.
- `build/storage-smoke-dev/packages/mylite-storage/mylite_storage_test`
  passed.
- `ctest --test-dir build/storage-smoke-dev --output-on-failure` passed
  `10/10` tests.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=updates 1000 100000`
  after the final full test run measured `15.002 us/op` direct primary-key
  updates and `9.791 us/op` prepared primary-key updates in one transaction.
  Earlier isolated reruns measured `15.487 us/op` direct / `11.880 us/op`
  prepared and `18.873 us/op` direct / `10.420 us/op` prepared. Benchmark
  variance remains high enough that the next slices should keep targeting the
  pager/write path rather than treating this as a stable threshold.
- `git diff --check` passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/include/mylite/storage.h packages/mylite-storage/tests/storage_test.c mariadb/storage/mylite/ha_mylite.cc`
  reported no formatting changes.

## Acceptance Criteria

- Durable SQL updates that do not change key bytes avoid redundant index-entry
  page writes.
- Exact index and live index scans resolve unchanged inherited keys to the
  current row id.
- Changed keys hide the inherited old key even when an older leaf root supplies
  the base index entry.
- Existing full-entry update callers remain correct.
- Storage and embedded storage-engine tests remain green.

## Risks And Open Questions

- This still leaves one row page and one row-state page per durable update.
  SQLite-like update throughput still needs a proper pager/WAL and maintained
  navigable index pages.
- Comparing old and new MariaDB key images adds CPU work in the handler, but
  current profiling shows physical writes dominate the benchmarked update path.
