# Dirty Page Buffer Leaf Pressure Bias

## Problem

Dirty-page buffer flush family counters show buffer-limit pressure still
publishes branch ancestors during prepared inserts:

- `index-leaf`: `54,341` buffer-limit pages.
- `index-branch`: `6,946` buffer-limit pages.
- statement commit: `1` branch page.

Branch pages are likely to remain hot across adjacent inserts because they are
shared ancestors for many leaf updates. The round-robin pressure cursor can
publish those branch pages even when an index leaf is also buffered, forcing
later branch rewrites to rebuffer and refresh checksums again.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- `store_dirty_page_in_buffer()` chooses the pressure flush slot when
  `MYLITE_STORAGE_DIRTY_PAGE_BUFFER_LIMIT` is reached.
- `flush_dirty_page_buffer_entry()` publishes one pressure-selected entry and
  leaves the rest of the dirty buffer live.
- `is_index_leaf_page()` identifies maintained index leaf pages from the page
  magic and page type without a full decode.

## Design

Keep the fixed dirty-page buffer size and one-page pressure eviction. Change
only pressure slot selection:

- start scanning at the existing pressure cursor;
- if a buffered index leaf is present, evict the first leaf found in that
  cursor-ordered scan;
- otherwise fall back to the existing cursor slot;
- overwrite the evicted slot with the new dirty page; and
- advance the cursor after the selected slot, preserving the existing progress
  rule.

The goal is to keep branch/root ancestors buffered when pressure can evict a
leaf instead. Whole-buffer statement commit and test-hook flush behavior is
unchanged.

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
journal companion behavior is unchanged. The selected pressure page has already
had its rollback image and journal protection handled before buffering.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API or durable file-format change. Binary-size impact is a
small internal selector helper and test-hook coverage. No dependency or license
change.

## Test And Verification Plan

- Add storage test-hook coverage with a full dirty buffer containing a branch at
  the cursor and a leaf later in the window, proving pressure evicts the leaf
  and keeps the branch buffered.
- Run the prepared-insert benchmark and compare branch pressure pages with the
  prior `6,946` reference.
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

- Buffer-limit pressure prefers an index leaf over a branch/root page when both
  are buffered.
- Root statement commit and test-hook whole-buffer flush behavior remains
  intact.
- Prepared-insert benchmark evidence records branch pressure-page impact.
- Existing storage and embedded storage-engine tests pass.

## Verification

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c`:
  passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed,
  `288.68 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`:
  passed, `32.40 MiB` archive.
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`:
  passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`:
  passed, `299.48 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`:
  passed. Prepared insert step was `62.404 us/op`, commit was `46.983 ms`.

Counter evidence improved from the prior family-counter run:

- buffer-limit index-branch flush pages dropped from `6,946` to `0`;
- buffer-limit index-leaf flush pages changed from `54,341` to `54,432`;
- statement-commit branch pages changed from `1` to `2`;
- total buffer-limit flush pages dropped from `61,287` to `54,432`;
- dirty-page-flush checksum refreshes dropped from `61,049` to `54,433`;
- insert-loop dirty refreshes dropped from `72,595` to `66,051`;
- total zero-tail checksum calls dropped from `223,842` to `217,299`; and
- insert-loop zero-tail checksum calls dropped from `113,990` to `107,446`.

## Risks

If leaf pages are as hot as branch pages in the active workload, leaf-biased
eviction may move pressure rather than reducing it. The benchmark family table
must decide whether the policy is worth keeping.
