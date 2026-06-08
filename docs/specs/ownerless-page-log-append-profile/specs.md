# Ownerless Page-Log Append Profile

## Problem

After the ownerless page-write refresh negative cache, the reduced ownerless
autocommit insert probe still spends a large fraction of time publishing page
versions. The existing performance probe reports the outer
`page_publish_hook_append_ms` bucket, but that bucket includes several
different costs:

- byte-range append lock acquisition,
- page-log header validation,
- file-size discovery,
- page-image checksum calculation,
- payload write,
- record-header write.

The next publish optimization needs to know which of those dominates before
changing the durable page-version WAL format, append locking, or checksum
policy.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- Slice start branch head: `e2074a3b`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` publishes one page-version image
  for each ownerless modified MTR page through
  `mylite_ownerless_innodb_publish_page_version()`.
- `packages/libmylite/src/database.cc::ownerless_innodb_page_publish_hook()`
  measures the outer append bucket around
  `mylite_ownerless_page_log_append_at()`.
- `packages/libmylite/src/ownerless_page_log.cc::mylite_ownerless_page_log_append_at()`
  acquires the page-log append lock, validates or creates the page-log header,
  calls `append_locked()`, then releases the append lock.
- `append_locked()` discovers the current file size with `fstat()`, computes a
  record checksum over the full page image, writes the page payload first, and
  writes the record header last. Writing the header last keeps incomplete
  trailing payloads invisible to readers.

## Design

Add opt-in internal page-log append performance counters in
`ownerless_page_log.cc`:

- append calls,
- total append time,
- append-lock acquire time,
- header validation/create time,
- append-body time,
- append-body `fstat()` time,
- payload checksum time,
- payload write time,
- record-header write time.

The counters are disabled by default and are enabled by the existing
stats-enabled embedded performance probe mode
(`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`). They are internal probe hooks,
not public `libmylite` API.

This slice intentionally does not change the page-log append format, checksum,
locking, write order, index publication, or recovery behavior.

## Compatibility Impact

No SQL, public C API, PHP API, storage format, durable metadata, or directory
layout changes. Existing page-version WAL readers and writers keep the same
record format and validation policy.

## Directory And Lifecycle Impact

No new files or durable state. Counters are process-local atomics reset by the
performance probe between measured ownerless insert phases.

## Native Storage Impact

No native storage behavior changes. Page-version append still writes the
payload before the record header, computes the existing checksum, and relies on
the same recovery and checkpoint rules.

## Build And Performance Impact

Default runtime cost is a relaxed boolean check before each counter update.
Timing reads and atomic additions run only when the probe enables append stats.

The expected result is actionable attribution inside the existing
`page_publish_hook_append_ms` bucket.

## Performance Findings

Local profiling on 2026-06-08 at slice start head `e2074a3b`, with
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, reported:

- ordinary cold create/open/close: `734.703ms`,
- ordinary warm open/close: `341.229ms`,
- ownerless warm open/close: `496.902ms`,
- ordinary direct `SELECT 1`: `4827.40 ops/s`,
- ordinary prepared `SELECT 1`: `2481.72 ops/s`,
- ordinary transactional inserts: `1692.01 ops/s`,
- ordinary autocommit inserts: `1676.09 ops/s`,
- ownerless direct `SELECT 1`: `3375.08 ops/s`,
- ownerless prepared `SELECT 1`: `2204.84 ops/s`,
- ownerless transactional inserts: `1485.64 ops/s`,
- ownerless autocommit inserts: `111.33 ops/s`.

For 200 ownerless transactional inserts, the page-log append path published
`209` records and took `14.355ms` total. The dominant append substages were
checksum calculation at `7.164ms` and payload writes at `5.061ms`; append-lock
acquire (`0.334ms`), header validation (`0.690ms`), `fstat()` (`0.177ms`), and
record-header writes (`0.388ms`) were small.

For 200 ownerless autocommit inserts, the page-log append path published
`1602` records and took `150.829ms` total. The substage breakdown was:

- append lock: `3.877ms`,
- header validation/create: `5.135ms`,
- append body: `138.205ms`,
- append-body `fstat()`: `1.697ms`,
- payload checksum: `86.024ms`,
- payload write: `46.033ms`,
- record-header write: `3.279ms`.

The outer hook's append bucket reported `151.361ms`, which matches the new
internal append total. The same autocommit run also reported page-read total
`173.036ms`, page-write refresh `163.574ms`, and page-write publish
`169.373ms`. The next optimization target should therefore not be lock/header
or `fstat()` overhead. The measured candidates are:

- reduce redundant page-version refresh/read work, because refresh and read
  cost are each comparable to append;
- reduce page-log checksum and payload-write cost, because those dominate the
  append bucket;
- evaluate append batching or page-image coalescing only with active-reader
  and recovery proof, because the current WAL header-last order is part of the
  crash-safety contract.

## Test Plan

- Build `mylite_embedded_performance_probe`.
- Run the stats-enabled embedded performance probe and verify page-log append
  substage counters are emitted for transactional and autocommit ownerless
  insert phases.
- Run focused live visibility selectors:
  `prepared-committed-read` and `local-write-first-read`.
- Run focused DDL/CTAS guard selectors:
  `ctas-post-create-dml`, `ddl-broader`, and `online-ddl-options`.
- Run focused ownerless primitive CTests.
- Run `format-check` and `git diff --check`.

## Verification Results

Local verification on 2026-06-08:

- `cmake --build --preset embedded-dev --target
  mylite_embedded_performance_probe mylite_ownerless_primitives_test
  mylite_ownerless_cross_process_sql_test` passed.
- `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/embedded-dev/packages/libmylite/mylite_embedded_performance_probe`
  passed and emitted the page-log append counters recorded above.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  prepared-committed-read` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  local-write-first-read` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  ctas-post-create-dml` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  ddl-broader` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  online-ddl-options` passed.
- `ctest --preset embedded-dev -L compat.ownerless-primitives
  --output-on-failure` passed two tests.
- `cmake --build --preset format-check` passed.
- `git diff --check` passed.

## Acceptance Criteria

- The stats-enabled probe emits page-log append call, lock, header, body,
  `fstat`, checksum, payload write, and record-header write counters.
- Counters reset between transactional and autocommit insert phases.
- Focused ownerless live-reader and DDL/CTAS guard selectors pass.
- Ownerless primitive CTests pass.
- The spec records the measured append breakdown and identifies the next
  optimization target.

## Risks And Follow-Up

- Instrumentation does not itself close the remaining ownerless autocommit
  performance gap.
- If checksum calculation dominates, a future slice can evaluate a format-aware
  faster checksum or a compatibility-preserving dual-checksum reader.
- If lock/header/fstat overhead dominates, a future slice can evaluate a
  runtime-prevalidated append path or append batching without weakening
  cross-process append ordering.
- Because page-read and page-write refresh costs remain comparable to append,
  checksum-only optimization will be useful but will not by itself close the
  ownerless autocommit gap.
