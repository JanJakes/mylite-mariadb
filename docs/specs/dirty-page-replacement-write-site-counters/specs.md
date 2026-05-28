# Dirty Page Replacement Write Site Counters

## Problem

The prepared-insert benchmark now shows that branch leaf refreshes reuse the
planned child offset (`122,388` offset hits and `0` refresh scans in the latest
storage-smoke run), but the remaining dirty-buffer replacement counters are
still high: `129,541` `index-branch` replacements, `64,881` `index-leaf`
replacements, and most of those replacements are checksum-dirty. The current
replacement table is grouped only by page family, so it does not identify which
maintained writer is repeatedly rewriting pages already resident in the dirty
page buffer.

Before changing dirty-buffer write policy or branch rewrite coalescing, the
next slice should attribute those replacements by maintained write site in the
same way the existing pressure and undo-copy counters attribute their hot
paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB-facing routed inserts enter MyLite storage through
  `mariadb/storage/mylite/ha_mylite.cc`; this slice does not change handler
  routing or SQL semantics.
- `packages/mylite-storage/src/storage.c` records replacement page families in
  `record_dirty_page_buffer_replacement_page()` when
  `store_dirty_page_in_buffer_at_pressure_write_site()` overwrites an existing
  buffered page.
- The same dirty-buffer entry already carries a test-hook-only
  `pressure_write_site_name` so nested dirty-buffer merges can preserve
  maintained writer attribution across pressure flushes.
- The prepared-insert benchmark prints replacement counts only by page family
  in `tools/mylite_perf_baseline.c`, which is not enough to choose the next
  rewrite or coalescing target.

## Design

- Add test-hook-only replacement write-site slots parallel to the existing
  pressure write-site slots.
- Reuse the write-site name already passed into
  `store_dirty_page_in_buffer_at_pressure_write_site()` and stored on dirty
  buffer entries during nested statement merges.
- When an existing dirty-buffer entry is replaced, record page family,
  checksum-dirty state, and replacement write-site name.
- Keep the existing family-level replacement counters unchanged.
- Print a new prepared-insert replacement write-site table in the benchmark.

This slice is instrumentation only. It does not change dirty-buffer eviction,
checksum-dirty handling, page publication, rollback, or file format.

## Affected Subsystems

- MyLite storage dirty-page buffer test hooks.
- Prepared-insert benchmark output.
- Storage test-hook coverage.

No MariaDB SQL-layer, handler registration, storage-engine routing, or public
`libmylite` API changes are planned.

## Compatibility Impact

No SQL-visible behavior, metadata behavior, storage-engine routing behavior, or
public API behavior changes. Routed `ENGINE=InnoDB` continues through MyLite
storage under the existing handler path.

## Single-File And Lifecycle Impact

Durable state remains in the primary `.mylite` file. The change records only
thread-local development counters in test-hook builds. Journal, rollback,
dirty-buffer flushing, locks, and file lifecycle behavior are unchanged.

## Public API, File-Format, Binary-Size, And Dependency Impact

No public API, durable file-format, dependency, or license change. Binary-size
impact is limited to test-hook counters and benchmark printing in development
builds.

## Test And Verification Plan

- Extend storage test-hook coverage so replacing an existing dirty buffered
  page under a named write site records:
  - the existing replacement page-family count;
  - the replacement write-site slot name;
  - per-family replacement and checksum-dirty replacement counts.
- Run the prepared-insert benchmark and verify replacement write-site rows are
  present for the remaining dirty-buffer replacement hot paths.
- Keep the existing compatibility and smoke tests passing:
  - `git diff --check`
  - `git clang-format --diff HEAD -- packages/mylite-storage/src/storage.c packages/mylite-storage/tests/storage_test.c tools/mylite_perf_baseline.c`
  - `cmake --build --preset dev --target mylite_storage_test`
  - `ctest --test-dir build/dev -R mylite-storage --output-on-failure`
  - `BUILD_DIR=build/mariadb-mylite-storage-smoke tools/mariadb-embedded-build all -DPLUGIN_MYLITE_SE=STATIC`
  - `cmake --build --preset storage-smoke-dev --target mylite_perf_baseline mylite_storage_test mylite_embedded_storage_engine_test`
  - `ctest --test-dir build/storage-smoke-dev -R 'mylite-storage|libmylite.embedded-storage-engine' --output-on-failure`
  - `build/storage-smoke-dev/tools/mylite_perf_baseline --phase=prepared-insert-components 1000 100000`

## Acceptance Criteria

- Dirty-buffer replacement counters remain available by page family.
- Test-hook builds also expose replacement counts by maintained write site and
  page family.
- Nested dirty-buffer merges preserve replacement write-site attribution.
- Prepared-insert benchmark output reports replacement write-site rows.
- Existing storage and routed embedded storage-engine tests pass.

## Risks

Replacement attribution must stay development-only and must not affect
production dirty-buffer entry layout outside test-hook builds. If more write
sites appear than the fixed slot limit, excess sites can be omitted from the
instrumentation rather than changing runtime behavior.
