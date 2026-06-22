# Ownerless MTR Commit Deep Attribution

## Problem

The ownerless production performance probe narrowed the remaining bulk-insert
gap after the page-image last-hit cache to MariaDB native row and undo mini-
transaction work. The broad hot buckets are:

- `row_ins_clust_low_mtr_commit`
- `trx_undo_report_mtr_commit`

Those buckets wrap `mtr_t::commit()`, so they do not distinguish redo write,
freed-page processing, flush-list insertion, ownerless redo leave, ownerless
page publication, memo release, or final resource cleanup. Optimizing native
undo, redo, or MTR behavior without that split risks skipping state required
for rollback, MVCC reads, crash recovery, or page-version visibility.

## Source Findings

MariaDB base: 11.8 LTS, imported from `mariadb-11.8.6`.

Relevant source paths:

- `mariadb/storage/innobase/row/row0ins.cc`
  - `row_ins_clust_index_entry_low()` measures clustered-row insert work and
    wraps its row-level `mtr.commit()` in
    `ROW_INS_CLUST_LOW_MTR_COMMIT_NS`.
- `mariadb/storage/innobase/trx/trx0rec.cc`
  - `trx_undo_report_row_operation()` writes undo records and wraps its undo
    `mtr.commit()` in `TRX_UNDO_REPORT_MTR_COMMIT_NS`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  - `mtr_t::commit()` runs `do_write()`, `process_freed_pages()`,
    `commit_log()`, and `release_resources()`.
  - `mtr_t::commit_log()` handles dirty flush-list insertion, redo latch
    release, ownerless redo leave, ownerless page publication, memo release,
    and the no-dirty release loop.

## Design

Add disabled-by-default deep performance counters that attribute
`mtr_t::commit()` and `mtr_t::commit_log()` phases while preserving existing
behavior:

- global MTR commit call counts,
- logged versus release-only commit counts,
- total MTR commit time,
- redo write, freed-page processing, commit-log, and resource-cleanup time,
- commit-log total time,
- dirty flush-list, latch release, ownerless redo leave, ownerless publish,
  memo release, and no-dirty loop time,
- dirty versus no-dirty commit-log call counts.

The counters are populated only when
`mylite_ownerless_innodb_deep_perf_stats_enabled` is enabled by the performance
probe. Normal production execution keeps the existing fast path.

The embedded production performance probe mirrors the new counter slots and
prints bulk first/remaining phase summaries so the ownerless-minus-ordinary
bulk gap can be split below the broad row/undo MTR buckets.

## Compatibility Impact

No SQL, C API, storage-engine, directory-layout, WAL-format, lock-file, redo,
checkpoint, page-version, or recovery semantics change. This is diagnostic
instrumentation only.

## Native Storage Impact

The instrumentation observes native InnoDB MTR phases but does not change
InnoDB page contents, page LSN assignment, undo records, redo records, flush
lists, checkpoint policy, page publication, or lock lifetime.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive with production baseline settings.
- Build the production embedded performance probe and focused ownerless SQL
  test target.
- Run a reduced production deep-stat probe and confirm the new
  `mtr_commit_*` first/remaining summary rows are emitted.
- Run focused ownerless SQL selectors that cover native-support page WAL
  elision and visible-fast path behavior.
- Run ownerless stress because the instrumentation touches the central MTR
  commit path.
- Run production-build guards, format-check, and `git diff --check`.

## Acceptance Criteria

- New counters compile in MariaDB-derived code and are mirrored by the C probe.
- Reduced production probe output exposes the MTR commit phase breakdown.
- Focused ownerless correctness tests still pass.
- Ownerless stress still passes.
- Docs and compatibility matrix state that this is attribution, not a
  completion or optimization claim.

## Verification

Local verification on the implementation slice:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
- Reduced production deep probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=1
  MYLITE_PERF_INSERT_ITERATIONS=500
  MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
  MYLITE_PERF_OWNERLESS_INNODB_DEEP_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- Focused ownerless CTest selector:
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|native-support-page-wal-elision|history-wal-proof)|uncommitted-peer-hidden)$'
  --output-on-failure`
- Direct ownerless SQL checks:
  `mylite_ownerless_cross_process_sql_test active-reader-pressure`
  and `mylite_ownerless_cross_process_sql_test commit-race`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test`
- `ctest --preset ownerless-stress --output-on-failure`
- `tools/check-ci-production-builds`
- `tools/require-cmake-release-build build/php-embedded-prod
  build/ownerless-stress`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The reduced deep probe reported the later non-empty-table ownerless bulk MTR
commit delta at `0.934 ms/statement`, with `0.607 ms/statement` in
`do_write()` and `0.310 ms/statement` in `commit_log()`. Inside commit-log, the
largest ownerless deltas were ownerless redo leave (`0.113 ms/statement`), the
no-dirty loop (`0.120 ms/statement`), and ownerless publish
(`0.063 ms/statement`). These numbers are local profiling evidence, not a
compatibility guarantee.

## Risks And Unresolved Questions

- Timer calls add overhead when deep stats are explicitly enabled; this is
  acceptable for profiling runs and disabled in normal execution.
- The new counters are global MTR attribution for the measured statement
  window. They explain which `mtr_t::commit()` phases dominate the window, but
  they do not yet tag each phase as row-level versus undo-level. If that split
  remains ambiguous, a later slice can add context tags at the row/undo call
  sites.
- Broad native undo/MTR elision remains out of scope until the attribution
  proves a safe sub-path.
