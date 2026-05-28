# Dirty Page Pressure Incoming Counters

## Problem

Prepared-insert benchmark evidence after the level-two writer cache-read slice
still reports:

- buffer-limit dirty-page flushes: `54,432`;
- buffer-limit dirty-page flush pages: `54,432`;
- buffer-limit flushed page family: `index-leaf = 54,432`;
- `dirty-page-flush / index-leaf` checksum refreshes: `54,432`.

Existing counters show which page family is evicted by pressure, but they do
not show which incoming page family forced the eviction. That leaves the next
optimization ambiguous: the writer may be evicting leaves to admit more leaves,
or it may be sacrificing leaves to keep branch/root pages resident.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- MyLite maintained-index page rewrites reach
  `pager_write_maintained_insert_page()` and
  `pager_write_buffered_maintained_index_page()`.
- `buffer_dirty_page_for_pager_write()` routes bufferable maintained-index
  pages into `store_dirty_page_in_buffer()`.
- `store_dirty_page_in_buffer()` replaces an existing entry when the page is
  already buffered; otherwise, when the dirty-page buffer is full, it calls
  `dirty_page_buffer_pressure_flush_index()` and then
  `flush_dirty_page_buffer_entry()`.
- Existing dirty-page buffer counters record the pressure source and flushed
  victim page family, but not the incoming page family or whether that incoming
  page was checksum-dirty.

## Design

Add test-hook counters for pages that are admitted after a buffer-limit
pressure flush:

- total incoming pressure pages by page family;
- checksum-dirty incoming pressure pages by page family.

Record the incoming page after the pressure flush succeeds and before the
flushed slot is replaced. Reuse the same page-family classifier used by
dirty-page buffer flush counters so the victim and incoming tables are directly
comparable.

Expose the counters through storage test hooks and print them in
`mylite_perf_baseline` near the existing dirty-page buffer flush tables.

## Affected Subsystems

- MyLite storage test hooks.
- Prepared-insert performance benchmark output.
- Dirty-page buffer pressure observability.

No MariaDB SQL-layer or handler-layer code changes are planned.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. The slice only records test-hook counters
inside the existing pressure path.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Journal, rollback, dirty
buffer, and active statement lifetimes are unchanged.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API, durable file-format, dependency, or license change.
Binary-size impact is limited to test-hook counters and benchmark output in the
development/test builds.

## Test And Verification Plan

- Extend dirty-page buffer pressure tests to assert incoming page-family
  counters for a pressure admission.
- Run the prepared-insert benchmark and compare pressure incoming families with
  pressure victim families.
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

- Buffer-limit pressure admissions record incoming page-family counts.
- Buffer-limit pressure admissions separately record incoming checksum-dirty
  page-family counts.
- The prepared-insert benchmark prints the incoming pressure table next to the
  existing flush victim table.
- Existing storage behavior and compatibility tests pass.

## Verification Results

Executed on the VPS after implementation:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `362.02 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed;
  embedded archive `33,970,026` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed
  in `325.41 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed.

Benchmark evidence:

- Prepared insert step component: `64.556 us/op`.
- Prepared insert commit component: `58.290 ms`.
- Buffer-limit flush pages: `54,432`, all `index-leaf`.
- Pressure incoming `index-leaf` pages: `54,289`, all checksum-dirty.
- Pressure incoming `index-branch` pages: `143`, with `105` checksum-dirty.
- `dirty-page-flush / index-leaf`: `54,432`.
- Branch and leaf writer fallback decodes remain `0`.

The pressure path is dominated by checksum-dirty leaf-to-leaf churn rather than
branch pages forcing leaf eviction.

## Risks

The counters may show that pressure is leaf-to-leaf churn, in which case the
next optimization likely needs a deeper buffering or maintained-index write
shape change rather than another local eviction-policy tweak.
