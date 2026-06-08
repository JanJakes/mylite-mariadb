# Ownerless History Flush Page Count

## Problem

The targeted rollback-segment-space flush improved stats-off ownerless
autocommit throughput locally, but the stats-enabled probe still shows the
write-history flush as a major per-insert cost. The current deep perf output
reports elapsed time for that flush, not how much native page work the flush
actually performed. Without a page count, the next decision is ambiguous: a
single expensive rollback-segment page handoff points toward a risky
undo/history page-version proof, while multiple pages per insert would point
toward reducing extra dirty-page churn.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `buf_flush_wait_space_flushed()` now loops through
  `buf_flush_list_space()` for one tablespace and already receives the
  `n_flushed` page count from that MariaDB helper.
- `trx_t::write_serialisation_history()` records ownerless history flush time
  through the existing `mylite_ownerless_innodb_deep_perf` counters.
- `packages/libmylite/tests/embedded_performance_probe.c` already emits raw
  deep counter values and compact per-insert summaries for ownerless
  autocommit samples.

## Design

Return the accumulated flushed-page count from the space-scoped flush helper
and ownerless wrapper. Add one deep perf counter for the write-history
ownerless flush page count, and emit both the raw counter and a per-insert
summary key:

- `*_trx_commit_persist_write_history_ownerless_flush_pages`
- `mylite_perf_summary_ownerless_autocommit_write_history_ownerless_flush_pages_per_insert`

No behavior changes are made. The counter is populated only when the existing
deep perf instrumentation is enabled.

## Compatibility Impact

No SQL, C API, PHP API, directory layout, native storage, or ownerless
coordination behavior changes. This is internal performance instrumentation.

## Native Storage Impact

No native page, redo, undo, or checkpoint format change. The same
rollback-segment-space flush still runs before releasing the ownerless
history page-write lock.

## Binary Size Impact

No new dependency. The embedded MariaDB archive gains one additional deep perf
counter update on the opt-in performance path.

## Test Plan

- Rebuild the MariaDB embedded archive and production embedded performance
  probe.
- Run a reduced stats-enabled production embedded performance probe and verify
  the new raw and summary page-count keys are present.
- Run a focused production ownerless SQL commit selector to ensure behavior is
  unchanged.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- The space-scoped history flush returns its accumulated native flushed-page
  count without changing the wait boundary.
- The performance probe prints raw and per-insert page-count keys.
- Focused production ownerless correctness coverage still passes.

## Verification Results

Local verification on 2026-06-08 used production embedded builds:

- `tools/mariadb-embedded-build build` passed and rebuilt
  `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- A reduced stats-enabled production probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=80`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. It reported
  `mylite_perf_ownerless_insert_autocommit_innodb_deep_trx_commit_persist_write_history_ownerless_flush_pages=201`
  and
  `mylite_perf_summary_ownerless_autocommit_write_history_ownerless_flush_pages_per_insert=2.513`,
  with the ownerless history flush at `0.814 ms/insert`.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  commit-race` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Follow-Up

- This only attributes the remaining cost. It intentionally does not skip or
  batch rollback-segment history flushes.
