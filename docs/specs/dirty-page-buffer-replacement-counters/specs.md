# Dirty Page Buffer Replacement Counters

## Problem

The prepared-insert dirty-page pressure counters show buffer-limit pressure is
mostly checksum-dirty leaf-to-leaf churn:

- pressure incoming `index-leaf` pages: `54,289`, all checksum-dirty;
- pressure incoming `index-branch` pages: `143`, with `105` checksum-dirty;
- buffer-limit victims: `54,432`, all `index-leaf`.

Before changing maintained leaf checksum timing, MyLite needs to know how often
a page already resident in the dirty-page buffer is rewritten before eviction.
If replacements are common, eagerly checksumming every leaf write could increase
work. If replacements are rare, prevalidating at admission may be a viable next
optimization.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB prepared inserts reach MyLite through
  `mariadb/sql/sql_insert.cc::Write_record::single_insert()`,
  `mariadb/sql/handler.cc::handler::ha_write_row()`, and
  `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()`.
- MyLite maintained-index page writes reach
  `buffer_dirty_page_for_pager_write()` and then
  `store_dirty_page_in_buffer()`.
- `store_dirty_page_in_buffer()` has three relevant paths:
  - replace an existing buffered entry;
  - flush one entry under buffer-limit pressure and reuse its slot;
  - append a new entry while capacity remains.
- Existing pressure counters cover the pressure-admission path but do not
  measure existing-entry replacements.

## Design

Add test-hook counters for dirty-page buffer replacements:

- replacement pages by page family;
- checksum-dirty replacement pages by page family.

Record the replacement after the existing entry is updated. Reuse the existing
dirty-page buffer page-family classifier so replacement, pressure incoming, and
flush victim tables use the same family names.

Expose the counters through storage test hooks and print them in
`mylite_perf_baseline` near the dirty-page buffer pressure tables.

## Affected Subsystems

- MyLite storage test hooks.
- Prepared-insert performance benchmark output.
- Dirty-page buffer observability.

No MariaDB SQL-layer or handler-layer code changes are planned.

## Compatibility Impact

No SQL-visible behavior, public API behavior, storage-engine routing behavior,
or file lifecycle behavior changes. The slice only records test-hook counters
inside the existing dirty-page buffer replacement path.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. Journal, rollback, dirty
buffer, and active statement lifetimes are unchanged.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public MyLite API, durable file-format, dependency, or license change.
Binary-size impact is limited to test-hook counters and benchmark output in the
development/test builds.

## Test And Verification Plan

- Add a storage test hook case that replaces an existing buffered index leaf and
  asserts replacement family and dirty-state counters.
- Run the prepared-insert benchmark and compare replacement counts with
  pressure incoming counts.
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

- Existing-entry dirty-page buffer replacements record page-family counts.
- Replacement counters separately record checksum-dirty replacement counts.
- The prepared-insert benchmark prints replacement counters near the pressure
  incoming table.
- Existing storage behavior and compatibility tests pass.

## Verification Results

Executed on the VPS after implementation:

- `git diff --check`: passed.
- `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`: passed.
- `cmake --build --preset dev --target mylite_storage_test`: passed.
- `ctest --test-dir build/dev -R mylite-storage --output-on-failure`: passed
  in `294.29 sec`.
- `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`: passed;
  embedded archive `33,970,026` bytes (`32.40 MiB`).
- `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`: passed.
- `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`: passed
  in `356.40 sec`.
- `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`: passed.

Benchmark evidence:

- Prepared insert step component: `92.606 us/op`.
- Prepared insert commit component: `44.564 ms`.
- Buffer-limit flush pages: `54,432`, all `index-leaf`.
- Pressure incoming `index-leaf` pages: `54,289`, all checksum-dirty.
- Replacement `index-leaf` pages: `64,881`, all checksum-dirty.
- Replacement `index-branch` pages: `129,541`, with `122,238`
  checksum-dirty.
- Replacement `index-root` pages: `666`, none checksum-dirty.
- Branch and leaf writer fallback decodes remain `0`.

The replacement counts show substantial repeated in-buffer rewrites. Eagerly
refreshing checksums on every buffered maintained-index write would likely
increase checksum work unless the policy is selective.

## Risks

The counters may show heavy replacement churn, making eager checksum refreshes
counterproductive without a more selective policy.
