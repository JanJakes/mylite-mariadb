# Dirty Page Buffer Clean Leaf Pressure Bias

## Problem

Prepared-insert evidence after leaf-biased dirty-page pressure eviction still
shows substantial checksum work in the insert loop:

- `54,432` buffer-limit flushes, all index-leaf pages.
- `54,432` dirty-page-flush checksum refreshes in the insert loop.
- `7,539` dirty-page-copy refreshes in the insert loop.

Dirty-page-copy refreshes clear a buffered page's checksum-dirty flag before a
later read. If pressure later evicts that already-checksummed leaf, the flush can
write it without another checksum refresh. The current pressure selector only
prefers any leaf over branch pages, so it may evict a checksum-dirty leaf while a
clean leaf is also buffered.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `copy_dirty_page_buffer()` refreshes a checksum-dirty buffered page before
  returning it to a reader and clears `entry->checksum_dirty`.
- `write_dirty_page_buffer_entry()` refreshes a buffered page only when its
  `checksum_dirty` flag is still set.
- `dirty_page_buffer_pressure_flush_index()` chooses the one page published when
  `MYLITE_STORAGE_DIRTY_PAGE_BUFFER_LIMIT` is reached.
- `is_index_leaf_page()` identifies maintained index leaf pages without running
  a full checksum-validating decode.

## Design

Keep the fixed dirty-page buffer size and the one-page pressure eviction policy.
Refine only the pressure slot selector:

- scan from the existing pressure cursor;
- remember the first buffered index leaf in cursor order;
- immediately select the first buffered index leaf whose checksum is already
  clean;
- if no clean leaf exists, fall back to the first leaf found; and
- if no leaf exists, fall back to the cursor slot.

The selector still prefers leaves over branch/root pages, preserving the prior
ancestor-retention policy. It only chooses a clean leaf ahead of a dirty leaf
when both are available.

## Affected Subsystems

- MyLite storage dirty-page buffer pressure eviction.
- Storage test-hook coverage for pressure slot selection.
- Prepared-insert performance benchmark evidence.

No MariaDB SQL-layer or handler-layer code changes are planned.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. Prepared inserts still route `ENGINE=InnoDB`
through MyLite storage.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Recovery and transaction
journal companion behavior is unchanged. The evicted page has already had its
rollback image and journal protection handled before buffering.

The change affects only which already-protected page is published when the
fixed in-memory dirty page window is full.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is a
small selector refinement and test-hook coverage. No dependency or license
change.

## Test And Verification Plan

- Add storage test-hook coverage with a full dirty buffer containing branch
  pages, a checksum-dirty leaf, and a checksum-clean leaf, proving pressure
  evicts the clean leaf and records no dirty-page-flush checksum refresh.
- Run the prepared-insert benchmark and compare dirty-page-flush refreshes with
  the prior `54,433` reference.
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

- Buffer-limit pressure prefers a checksum-clean index leaf over a
  checksum-dirty index leaf when both are buffered.
- Branch/root ancestor retention remains intact when any leaf is buffered.
- Root statement commit and test-hook whole-buffer flush behavior remains
  intact.
- Prepared-insert benchmark evidence records whether dirty-page-flush checksum
  refreshes improve from the prior `54,433` reference.
- Existing storage and embedded storage-engine tests pass.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `285.54 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed, `32.40 MiB` archive.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `358.49 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step was `104.016 us/op`, commit was `91.048 ms`.

Counter evidence on this VPS did not change for the canonical prepared-insert
run:

- buffer-limit flushes/pages remained `54,432` / `54,432`;
- buffer-limit flush pages remained `54,432` index-leaf pages;
- dirty-page-flush refreshes remained `54,433`;
- insert-loop dirty-page-flush refreshes remained `54,432`; and
- insert-loop index-leaf dirty refreshes remained `57,507`.

That means the focused test proves the selector behavior, but the benchmarked
prepared-insert workload did not have a checksum-clean leaf available at the
pressure cursor before the fallback dirty-leaf choice.

## Risks

The clean-leaf opportunity may be workload-dependent. If few copied leaves are
still buffered when pressure occurs, the benchmark may show little timing or
counter movement. The fallback policy remains the existing leaf-biased selector.
