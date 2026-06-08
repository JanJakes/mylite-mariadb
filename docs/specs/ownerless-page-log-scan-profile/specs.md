# Ownerless Page-Log Scan Profile

## Problem

After the page-log read-guard slice, the reduced ownerless autocommit probe
still performs thousands of negative page-version WAL scans. The existing
counters show scan calls and miss results, but they do not distinguish:

- a true page-key absence scan, where no retained WAL record exists for the
  requested `(space_id, page_no)`; from
- a same-page visibility miss, where same-page records exist but none are
  visible at `max_commit_lsn`.

That distinction matters for the next optimization. A cache that extends a
true page-key absence proof can be independent of commit-LSN growth within the
already-scanned range. A same-page visibility miss cannot be reused that way
because a later `max_commit_lsn` may make an already-scanned same-page record
visible.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:ownerless_innodb_page_read_locked()`
  records page-index hits, true index misses, WAL-scan calls, miss/found
  outcomes, and generation-bound negative-cache hits/stores.
- `packages/libmylite/src/ownerless_page_log.cc:find_latest_in_snapshot()`
  is the only layer that sees the scanned page-log record headers and can tell
  whether any same-page record was encountered before returning
  `MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND`.
- Previous unsafe shortcuts failed because page-index absence alone is not a
  durable proof for all CTAS/DDL file-lifecycle states. This slice does not
  change the proof; it only records the scan shape after the authoritative scan
  has run.

## Design

Add opt-in page-log scan counters next to the existing append counters:

- scan calls;
- scanned record headers;
- same-page records;
- visible same-page records;
- found scans;
- not-found scans with no same-page record;
- not-found scans with same-page records but no visible record;
- scan errors.

The counters are disabled by default and use relaxed atomics only when enabled.
The embedded performance probe enables, resets, reads, and prints them under
the existing `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` mode.

No scan branch changes the selected record, checksum validation, tail-record
tolerance, or return code. The instrumentation records local counters and
publishes them only at the same return points already used by the scanner.

## Compatibility Impact

No SQL, public C API, PHP API, storage format, durable directory layout, or
ownerless visibility semantics change. The new functions are internal
first-party diagnostic hooks used by the embedded performance probe.

## Directory And Lifecycle Impact

No new files and no database-directory changes. The counters are process-local
and reset only by the probe.

## Native Storage Impact

No native storage format changes.

## Build And Performance Impact

The counters are off by default. When enabled, `find_latest_in_snapshot()` adds
local counting plus relaxed atomic aggregation on exit. This can slightly
change measured timings in stats-enabled probe runs, so throughput should be
treated as a profiling sample rather than a production benchmark.

Measured on 2026-06-08 with
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`:

- transactional ownerless insert phase:
  - `4` WAL scans;
  - `203` scanned record headers;
  - `0` same-page records;
  - `4` not-found scans with no same-page record.
- autocommit ownerless insert phase:
  - `5776` WAL scans;
  - `1000` scanned record headers;
  - `0` same-page records;
  - `5776` not-found scans with no same-page record;
  - `0` same-page-not-visible misses.

The current reduced workload's repeated negative scans are therefore true
page-key absence scans, not commit-LSN visibility misses. A future optimization
can target an absent-page proof or incremental tail scan, provided it is
invalidated across page-log checkpoint/rewrite and remains guarded by the CTAS
and DDL selectors that rejected broad page-index miss shortcuts.

## Test Plan

- Build `mylite_embedded_performance_probe`,
  `mylite_ownerless_primitives_test`, and
  `mylite_ownerless_cross_process_sql_test`.
- Run `mylite_ownerless_primitives_test`.
- Run the stats-enabled embedded performance probe and record
  `page_log_scan_*` counters.
- Run focused ownerless selectors that rejected unsafe page-index shortcuts:
  `prepared-committed-read`, `local-write-first-read`,
  `ctas-post-create-dml`, `ddl-broader`, and `online-ddl-options`.
- Run `ctest --preset embedded-dev -L compat.ownerless-primitives
  --output-on-failure`.
- Run `format-check` and `git diff --check`.

## Verification

Completed on 2026-06-08:

- `tools/mariadb-embedded-build build` refreshed the embedded archive after an
  earlier reverted hook experiment had left the hook source timestamp newer
  than the archive.
- `cmake --build --preset embedded-dev --target
  mylite_embedded_performance_probe mylite_ownerless_primitives_test
  mylite_ownerless_cross_process_sql_test` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_primitives_test`
  passed before and after formatting.
- `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/embedded-dev/packages/libmylite/mylite_embedded_performance_probe`
  passed and produced the scan evidence recorded above.
- Focused ownerless selectors passed: `prepared-committed-read`,
  `local-write-first-read`, `ctas-post-create-dml`, `ddl-broader`, and
  `online-ddl-options`.
- `ctest --preset embedded-dev -L compat.ownerless-primitives
  --output-on-failure` passed.
- `cmake --build --preset format-check` passed after formatting
  `ownerless_page_log.cc` and `embedded_performance_probe.c`.
- `git diff --check` passed.

## Acceptance Criteria

- Page-log scan counters are disabled by default.
- Stats-enabled embedded probes emit page-log scan-shape counters for each
  ownerless insert phase.
- A `NOT_FOUND` scan is classified as either no same-page record or same-page
  record not visible.
- The scanner still returns the same page-version results and error codes.
- Focused page-version and CTAS/DDL selectors continue to pass.

## Risks And Follow-Up

- This slice is diagnostic. It does not reduce the scan count.
- The scan profile supports a future absent-page or incremental-tail cache, but
  that optimization must explicitly invalidate cache entries when checkpoint
  rewrites the page-version WAL.
- Broader native redo/checkpoint reconciliation, DDL/file lifecycle recovery,
  SQL-level table-lock fault injection, and external randomized MariaDB/RQG
  stress remain separate ownerless completion work.
