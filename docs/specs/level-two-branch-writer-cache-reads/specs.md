# Level-Two Branch Writer Cache Reads

## Problem

Dirty-refresh source/family counters show the prepared-insert timed loop still
refreshes dirty buffered pages for copy-for-read:

- `dirty-page-copy` / `index-branch`: `4,464`;
- `dirty-page-copy` / `index-leaf`: `3,075`;
- `dirty-page-flush` / `index-leaf`: `54,432`.

The level-two branch insert writer reads its root branch, selected child
branch, and selected leaf through `pager_read_page()` and then decodes them.
When those pages are dirty-buffered, `read_page_at()` must refresh the dirty
checksum before returning the copy. Existing branch insert writer helpers
already probe active branch and leaf page caches first, but this level-two path
does not use them.

The first implementation pass routed the level-two path through the helpers,
but the prepared-insert benchmark showed the `dirty-page-copy` branch and leaf
refresh counts did not move. That means the dirty-refresh counters are useful
for locating the next pressure point, but they do not prove these direct reads
were the active source. This slice therefore also adds direct branch/leaf writer
cache-hit counters so the route change is measurable.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `insert_level_two_branch_index_leaf_entry()` reads branch and leaf pages
  directly through `pager_read_page()`.
- `read_branch_insert_writer_branch_page()` and
  `read_branch_insert_writer_leaf_page()` already:
  - probe active statement page caches;
  - fall back to `pager_read_page()` plus checksum-validating decode when
    needed;
  - refresh active page caches after fallback reads; and
  - expose fallback decode counts through existing test hooks.
- The prepared-insert benchmark currently reports writer fallback decodes as
  `0`, so routing the level-two writer through the helpers should preserve that
  evidence if active cache hits cover the hot path.

## Design

Route the level-two branch insert writer's initial page reads through existing
writer helpers:

- read `insert->root_page_id` with `read_branch_insert_writer_branch_page()`;
- read `insert->child_branch_page_id` with the same branch helper; and
- read `insert->leaf_page_id` with `read_branch_insert_writer_leaf_page()`.

Keep all subsequent validation, branch-fence checks, leaf split/fitting logic,
dirty-buffer writes, and active cache refresh behavior unchanged.

Extend the existing writer read test hooks with separate branch and leaf
active-cache hit counters. Keep the existing fallback decode counters so the
benchmark can distinguish "served from active cache" from "helper fell back to
the old pager-read/decode path".

## Affected Subsystems

- MyLite maintained-index branch insert writer.
- Active branch/leaf page cache reuse on the prepared-insert hot path.
- Prepared-insert performance benchmark evidence and storage test hooks.

No MariaDB SQL-layer or handler-layer code changes are planned.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. The helpers perform the same checksum
validation on fallback reads and return cached page metadata only from the
active statement's existing page caches.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Recovery and transaction
journal companion behavior is unchanged. The slice only changes how the writer
obtains already-active page bytes before it rewrites protected pages.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is
limited to three existing-helper calls replacing direct read/decode sequences
plus test-hook counters compiled into the benchmark/test configuration. No
dependency or license change.

## Test And Verification Plan

- Keep existing storage coverage for active branch/leaf writer caches passing
  and assert that same-statement branch insert paths report writer cache hits
  with zero fallback decodes.
- Run the prepared-insert benchmark and compare dirty-page-copy
  `index-branch` / `index-leaf` counts, full-page branch/leaf checksum counts,
  writer cache-hit counters, and writer fallback decode counters.
- Keep storage and routed embedded storage-engine tests passing.
- Run:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- The level-two branch insert writer uses the existing active branch/leaf cache
  helper reads.
- Writer read counters report branch and leaf cache hits separately from
  fallback decodes.
- Existing branch insert behavior and storage tests pass.
- Prepared-insert benchmark evidence records writer cache hits and whether
  dirty-page-copy branch and leaf refreshes move without increasing writer
  fallback decode counters.

## Verification Results

Executed on the VPS after implementation:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `319.53 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed;
  embedded archive `33,970,026` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed
  in `325.74 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed.

Benchmark evidence:

- Prepared insert step component: `80.048 us/op`.
- Prepared insert commit component: `86.231 ms`.
- Branch insert writer branch cache hits: `122,388`.
- Branch insert writer leaf cache hits: `122,388`.
- Branch insert writer branch fallback decodes: `0`.
- Branch insert writer leaf fallback decodes: `0`.
- `dirty-page-copy / index-leaf`: `3,075`.
- `dirty-page-copy / index-branch`: `4,464`.
- `dirty-page-flush / index-leaf`: `54,432`.

The writer route is now directly visible as active-cache hits, but the
dirty-refresh and full-page checksum counters did not drop. The remaining
prepared-insert pressure is therefore outside these initial level-two writer
reads.

## Risks

If the active page caches are absent or invalidated before this writer path,
the helpers fall back to the previous pager read and decode behavior. In that
case, benchmark counters may move little, but behavior remains equivalent.
