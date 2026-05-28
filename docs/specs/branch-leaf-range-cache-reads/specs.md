# Branch Leaf Range Cache Reads

## Problem

The pager-read site counters identify the main remaining dirty-buffer copy
refresh source in the prepared-insert benchmark:

- `redistribute_branch_index_leaf_range_entry / index-leaf`: `3,000`
  checksum-dirty copies.
- `redistribute_branch_index_leaf_range_entry / index-branch`: `4,221`
  checksum-dirty copies.

The function reads the selected branch and child leaves through
`pager_read_page()` and then decodes them. When those pages are already active
and dirty-buffered in the statement, this forces copy-for-read checksum
refreshes that the branch writer helpers can avoid through active page caches.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `redistribute_branch_index_leaf_range_entry()` reads
  `insert->root_page_id` with `pager_read_page()`, then calls
  `decode_index_branch_page()`.
- The same function reads each selected child leaf with `pager_read_page()`,
  then calls `decode_index_leaf_page()`.
- `read_branch_insert_writer_branch_page()` and
  `read_branch_insert_writer_leaf_page()` already probe active branch/leaf
  page caches, fall back to `pager_read_page()` plus decode when needed, and
  publish cache-hit/fallback-decode test counters.
- `redistribute_level_two_branch_index_leaf_range_entry()` already uses the
  branch helper for its upper and child branch reads, then delegates to the
  single-level redistribution helper for the lower range.

## Design

Route single-level branch leaf-range redistribution through the existing writer
read helpers:

- read and decode `insert->root_page_id` with
  `read_branch_insert_writer_branch_page()`;
- read and decode each selected leaf with
  `read_branch_insert_writer_leaf_page()`;
- keep all table/index/key validation, duplicate child checks, entryset
  construction, leaf-page preparation, branch fence refresh, dirty-buffer
  writes, rollback, and journal behavior unchanged.

The helper fallback path still performs the old pager read plus decode. Active
cache hits avoid the dirty-buffer copy path entirely.

## Affected Subsystems

- MyLite maintained-index branch leaf-range redistribution.
- Active branch/leaf page cache reuse on the prepared-insert hot path.
- Prepared-insert performance benchmark evidence and storage test hooks.

No MariaDB SQL-layer or handler-layer code changes are planned.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. The helper readers perform the same page
decode validation on fallback reads and only use existing active statement
caches.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Journal, rollback,
statement dirty buffer, and active page cache lifetimes are unchanged.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API, durable file-format, dependency, or license change.
Binary-size impact is limited to replacing local read/decode sequences with
existing helper calls.

## Test And Verification Plan

- Keep branch leaf-range redistribution storage coverage passing.
- Run the prepared-insert benchmark and compare:
  - dirty-page-copy `index-leaf` / `index-branch` refreshes;
  - pager-read site rows for `redistribute_branch_index_leaf_range_entry`;
  - branch insert writer cache-hit and fallback-decode counters.
- Keep storage and routed embedded storage-engine tests passing.
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

- Single-level branch leaf-range redistribution uses the cache-aware branch
  and leaf writer readers.
- Existing leaf-range redistribution behavior and rollback coverage pass.
- Prepared-insert benchmark evidence shows whether
  `redistribute_branch_index_leaf_range_entry` dirty pager-read copies move
  without increasing helper fallback decode counts.

## Verification Results

The requested verification completed on the VPS:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`:
  passed in `322.12 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` is `33,969,962` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `365.21 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; prepared insert step measured `77.685 us/op`.

Benchmark evidence:

- `redistribute_branch_index_leaf_range_entry` no longer appears in the
  pager-read site table.
- Branch insert writer cache hits increased to `129,925` branch hits and
  `147,184` leaf hits.
- Branch insert writer fallback decodes stayed at `0` for both branch and
  leaf reads.
- `dirty-page-copy / index-branch` dropped from `4,464` to `243`.
- Total `dirty-page-copy` refreshes dropped from `7,539` to `3,318`.
- Full-page checksum calls dropped from `37,460` to `5,127`.
- `dirty-page-copy / index-leaf` remained `3,075`; `3,000` of those copies
  moved from pager-read to dirty-page-undo-capture context because the helper
  route no longer refreshes those dirty leaf pages before the subsequent pager
  writes capture undo.

## Risks

If the hot pages are not present in active caches for some workload shape, the
helpers will fall back to the old pager-read/decode path. That remains
correct, and the fallback decode counters will make the miss visible.
