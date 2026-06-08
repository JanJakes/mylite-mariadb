# Ownerless Page-Log Absence Tail Cache

## Problem

The page-log scan profile shows that the reduced ownerless autocommit probe
still performs thousands of authoritative WAL scans whose misses are true
page-key absence scans, not same-page commit-LSN visibility misses. The current
process-local negative cache is tied to the page-index generation, so unrelated
page-version publishes invalidate absence proofs and force later reads to scan
the already-proven prefix again.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:ownerless_innodb_page_read_locked()`
  caches a negative proof only for a matching page-index generation after an
  authoritative page-log scan returns `NOT_FOUND`.
- `packages/libmylite/src/ownerless_page_index.cc` increments that generation
  on every publish, clear, replace, or WAL-scan-required transition. That is a
  safe invalidation token, but it is too broad for true page-key absence.
- `packages/libmylite/src/ownerless_page_log.cc:find_latest_in_snapshot()`
  is the authority that can prove whether a scan saw zero same-page records.
- Page-log checkpoint rewrites retained records from the payload start and
  truncates the file. A cache that stores physical offsets must therefore be
  invalidated by a WAL-generation token, not by file size alone.

## Design

Add a reserved page-log header generation counter. New logs initialize it to
`1`; checkpoint and safe-truncate paths increment it before rewriting or
truncating retained records. Existing logs with a zero generation remain valid
and move to a nonzero generation on the first checkpoint.

Extend the internal page-log scan helper so callers that already hold the
page-log read guard can:

- snapshot the current append end and page-log generation;
- scan from a caller-provided record-boundary offset up to that snapshot end;
- report whether any same-page record was encountered before returning.

Change the ownerless read hook's negative cache from
`(page, page-index-generation, max_commit_lsn)` to
`(page, page-log-generation, covered_end_offset)`.

The read path uses the tail cache only for current page-index
`NOT_FOUND` results:

1. Snapshot the WAL end and generation under the existing page-log read guard.
2. If a matching cache entry covers the current snapshot end, return
   `UNAVAILABLE` without a WAL scan.
3. If a matching entry covers an earlier offset in the same generation, scan
   only the tail from that offset to the current snapshot end.
4. Store or extend the entry only when the authoritative full/tail scan returns
   `NOT_FOUND` and reports zero same-page records in the scanned range.

`SCAN_REQUIRED`, stale direct-index reads, errors, and scans that saw same-page
records still use the existing conservative full-scan behavior and do not
extend the absence proof.

## Compatibility Impact

No SQL, public C API, PHP API, storage format visible to applications, or
directory layout changes. The header generation uses reserved bytes in the
internal MyLite page-version WAL header.

## Directory And Lifecycle Impact

No new files. Cache entries are process-local and are discarded on close,
crash, process death, or hook reset. Peer checkpoints invalidate entries by
advancing the page-log generation before releasing the checkpoint write lock.

## Native Storage Impact

No native InnoDB storage format changes. Missing page versions still fall
through to MariaDB's native page path.

## Performance Evidence

Local stats-enabled probes on 2026-06-08 showed the first implementation
reduced scan counts but paid a WAL snapshot on every page-index miss, including
cache hits. That dropped autocommit throughput to `24.10 ops/s` despite reducing
WAL scans from the previous `5776` baseline to `3388`.

The final hybrid keeps the existing page-index-generation fast hit before
using the WAL-generation tail cache. A follow-up run reported:

- ordinary warm open/close: `418.452ms`;
- ownerless warm open/close: `452.809ms`;
- ordinary direct `SELECT 1`: `4574.99 ops/s`;
- ownerless direct `SELECT 1`: `3205.87 ops/s`;
- ordinary autocommit inserts: `1969.05 ops/s`;
- ownerless autocommit inserts: `87.82 ops/s`;
- ownerless autocommit page-read WAL scans: `3388`, down from the previous
  `5776`;
- negative-cache hits: `6123`, up from the previous `3735`;
- page-read WAL-scan time: `27.654ms`, down from the previous `73.216ms`;
- page-log scan record headers: `1000`, with zero same-page records and zero
  same-page-not-visible misses.

End-to-end ownerless autocommit remains noisy and still spends meaningful time
in page-write refresh and page-version publication, but the targeted repeated
absent-page scan work is reduced without changing the authoritative first
proof.

## Test Plan

- Add primitive coverage for page-log generation snapshots, generation advance
  on checkpoint, and same-generation tail scans.
- Build `mylite_embedded_performance_probe`,
  `mylite_ownerless_primitives_test`, and
  `mylite_ownerless_cross_process_sql_test`.
- Run `mylite_ownerless_primitives_test`.
- Run the stats-enabled embedded performance probe and compare
  `page_log_scan_record_headers`, WAL-scan calls, and negative-cache hits.
- Run focused selectors that rejected unsafe broad skips:
  `prepared-committed-read`, `local-write-first-read`,
  `ctas-post-create-dml`, `ddl-broader`, and `online-ddl-options`.
- Run ownerless primitive CTests, `format-check`, and `git diff --check`.

## Verification

Completed on 2026-06-08:

- `cmake --build --preset embedded-dev --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_primitives_test`
  passed before and after the final formatting pass.
- `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/embedded-dev/packages/libmylite/mylite_embedded_performance_probe`
  passed twice; the final hybrid-cache sample is recorded above.
- Focused ownerless selectors passed: `prepared-committed-read`,
  `local-write-first-read`, `ctas-post-create-dml`, `ddl-broader`, and
  `online-ddl-options`.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test
  stress` passed.
- `ctest --preset embedded-dev -L compat.ownerless-primitives
  --output-on-failure` passed before formatting. After formatting, the first
  rerun hit a transient InnoDB log-header checksum abort in
  `libmylite.embedded-ownerless-directory-lifecycle`; the same test passed in
  isolation and the full label rerun passed cleanly.
- `cmake --build --preset format-check` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Cache hits never depend on page-index absence alone; an authoritative scan
  must first prove zero same-page records through the cached offset.
- Tail scans are used only when the current WAL generation matches the cached
  proof.
- Checkpoint/rewrite paths advance the WAL generation.
- Focused page-version and CTAS/DDL selectors continue to pass.
- The stats-enabled probe shows fewer scanned page-log record headers for the
  reduced ownerless autocommit workload.

## Risks And Follow-Up

- This optimizes true absent-page scans only. Same-page older-snapshot cases
  still require a historical index or full WAL scan.
- Page-log append checksum/payload writes and page-write refresh remain
  separate ownerless autocommit costs.
- Broader native redo/checkpoint reconciliation, DDL/file lifecycle recovery,
  SQL-level table-lock fault injection, and external randomized MariaDB/RQG
  stress remain separate ownerless completion work.
