# Branch Leaf Split Cache Reads

## Problem

After routing branch leaf-range redistribution through cache-aware helper reads,
the prepared-insert benchmark's remaining pager-read dirty site rows are from
single-level branch leaf splits:

- `split_branch_index_leaf_entry / index-leaf`: `75` checksum-dirty copies.
- `split_branch_index_leaf_entry / index-branch`: `243` checksum-dirty copies.

`split_branch_index_leaf_entry()` reads the selected branch and full leaf
through `pager_read_page()` and then decodes them. Other branch writer paths use
the cache-aware branch insert writer readers, which can avoid copy-for-read
checksum refreshes when the pages are active in the statement.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `split_branch_index_leaf_entry()` reads `insert->root_page_id` with
  `pager_read_page()` and decodes the branch page.
- It then reads `insert->leaf_page_id` with `pager_read_page()` and decodes the
  leaf page before splitting the entries into two leaves.
- `read_branch_insert_writer_branch_page()` and
  `read_branch_insert_writer_leaf_page()` already probe active page caches,
  fall back to `pager_read_page()` plus decode when needed, and report
  cache-hit/fallback-decode counters.

## Design

Route single-level branch leaf split reads through the existing writer helpers:

- read and decode `insert->root_page_id` with
  `read_branch_insert_writer_branch_page()`;
- read and decode `insert->leaf_page_id` with
  `read_branch_insert_writer_leaf_page()`;
- keep split-child selection, leaf split preparation, branch child refresh,
  page writes, rollback, and validation unchanged.

The helper fallback path preserves the old pager-read/decode behavior when a
page is not active in cache.

## Affected Subsystems

- MyLite maintained-index single-level branch leaf splits.
- Active branch/leaf page cache reuse on the prepared-insert hot path.
- Prepared-insert benchmark evidence and storage test hooks.

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

- Keep existing branch leaf split, branch-refold cache split, and rollback
  storage coverage passing.
- Run the prepared-insert benchmark and compare:
  - pager-read site rows for `split_branch_index_leaf_entry`;
  - dirty-page-copy `index-leaf` / `index-branch` refreshes;
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

- Single-level branch leaf split reads use cache-aware branch and leaf writer
  readers.
- Existing leaf split behavior and rollback coverage pass.
- Prepared-insert benchmark evidence shows whether
  `split_branch_index_leaf_entry` pager-read site rows move without increasing
  helper fallback decode counts.

## Verification Results

The requested verification completed on the VPS:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`:
  passed in `375.39 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed; `libmariadbd.a` is `33,970,634` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed in `362.30 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed; prepared insert step measured `62.124 us/op`.

Benchmark evidence:

- `split_branch_index_leaf_entry` no longer appears in the pager-read site
  table.
- The pager-read site table now has only clean `index-root` copies from
  `write_maintained_index_root_inserts` and
  `write_maintained_index_root_overflow_flags`.
- Branch insert writer cache hits increased to `130,311` branch hits and
  `147,570` leaf hits.
- Branch insert writer fallback decodes stayed at `0` for both branch and
  leaf reads.
- Full-page checksum calls dropped from `5,127` to `4,355`.
- Total `dirty-page-copy` refreshes remained `3,318`, now all under
  dirty-page-undo-capture for `index-leaf` (`3,075`) and `index-branch`
  (`243`).

## Risks

As with leaf-range redistribution, avoiding a pager read can shift a dirty page
refresh to later undo capture if the following write still needs a before
image. That is acceptable for this slice; the purpose is to avoid redundant
read-path refreshes and make any remaining undo work explicit.
