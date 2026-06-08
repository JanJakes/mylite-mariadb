# Ownerless Page Index Scan-Required Classification

## Problem

The WordPress PHPUnit job now separates source fetch, native/PHP builds,
dependency installation, database preparation, performance probing, and the
PHPUnit suite. Focused WordPress timings stay close to trunk; the larger
ownerless-specific slowdown is inside the native ownerless autocommit write
path.

A stats-enabled embedded probe on 2026-06-08 at `d56ef872` reported:

- ordinary autocommit inserts: `2105.08 ops/s`;
- ownerless transactional inserts: `993.58 ops/s`;
- ownerless autocommit inserts: `76.55 ops/s`;
- ownerless autocommit page reads: `16546`;
- page-index hits: `600`;
- page-index misses: `15946`;
- WAL scans: `15946`;
- WAL-scan misses: `15946`;
- WAL-scan time: `264.031ms`.

Those WAL scans are authoritative. A broad page-index miss shortcut is unsafe:
focused `ctas-post-create-dml`, `ddl-broader`, and `online-ddl-options`
selectors reproduced CTAS/DDL stale-reader failures when the read path skipped
the WAL scan directly from index absence, even with an active-pin guard.

The next safe slice is therefore diagnostic, not speculative: split page-index
results so profiling can distinguish true absent entries from states that are
known to need the authoritative WAL scan.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/ownerless_page_index.cc` stores one latest
  page-version record per `(space_id, page_no)` while the
  `wal_scan_required` header bit is clear. On overflow or explicit reclaim
  preparation, `mylite_ownerless_page_index_require_wal_scan()` sets that bit.
- `mylite_ownerless_page_index_find()` used one `NOT_FOUND` result for three
  states: incomplete index, no page entry, and page entry newer than
  `max_commit_lsn`.
- `packages/libmylite/src/database.cc:ownerless_innodb_page_read_locked()`
  treats page-index absence as requiring
  `mylite_ownerless_page_log_find_latest_at()`.
- `packages/libmylite/src/ownerless_page_log.cc:find_latest_in_snapshot()`
  scans record headers from the page-log start to the snapshot end. Negative
  reads are O(number of retained page-version records), and the autocommit
  probe repeats that negative scan thousands of times.

## Rejected Designs

- Directly skipping the WAL scan on `NOT_FOUND` removed the measured WAL scans
  from the autocommit probe, but broke CTAS/DDL guard selectors. Page-index
  absence is not proof that no retained record is needed for every native file
  lifecycle state.
- A page-log generation plus thread-local negative cache was also explored. It
  still failed the CTAS/DDL guard selectors, and even the disabled-cache
  variant disturbed the page-log snapshot path. That approach is not part of
  this slice.

## Design

Split the page-index miss result into two meanings:

- `MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND`: the current index lookup did not find
  an entry for `(space_id, page_no)`.
- `MYLITE_OWNERLESS_PAGE_INDEX_SCAN_REQUIRED`: the index cannot prove the
  requested snapshot by itself, including `wal_scan_required` and the case where
  a page entry exists but the indexed latest version is newer than
  `max_commit_lsn`.

The database read path keeps the existing authoritative WAL scan for both
results. The only runtime behavior change is diagnostics: the opt-in embedded
performance counters now report `page_read_index_scan_required` separately from
true index misses.

This keeps the safety property intact while narrowing the remaining performance
problem. Future optimizations can target true `NOT_FOUND`, `SCAN_REQUIRED`, or
historical snapshot lookup separately, but they must prove correctness through
the CTAS/DDL selectors that rejected the broader shortcuts.

## Compatibility Impact

No SQL, public C API, PHP API, storage format, or durable directory layout
changes. The page-index primitive is internal ownerless runtime state.
Ownerless snapshot semantics remain conservative: page reads still scan the
page-version WAL whenever the index cannot prove the requested page version.

## Directory And Lifecycle Impact

No new files and no directory-layout changes. No durable header or page-log
format change is introduced by this slice.

## Native Storage Impact

No native storage format changes. Missing page versions still fall through to
the existing native tablespace read path in InnoDB's refresh hook.

## Build And Performance Impact

The slice does not claim a throughput win. It makes the ownerless autocommit
hot path measurable enough to avoid unsafe broad shortcuts. In particular, it
separates:

- true page-index absence, which may eventually support a carefully proven
  negative optimization;
- incomplete-index or older-snapshot states, which must use a historical index
  or WAL scan.

## Test Plan

- Update primitive page-index tests to assert scan-required results for
  incomplete indexes and older-snapshot lookups, while preserving true absent
  `NOT_FOUND`.
- Build `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test`.
- Run `mylite_ownerless_primitives_test`.
- Run a stats-enabled embedded performance probe and record
  `page_read_index_scan_required`.
- Run focused ownerless selectors that exposed unsafe broad miss shortcuts:
  `ctas-post-create-dml`, `ddl-broader`, and `online-ddl-options`.
- Run focused page-version selectors: `prepared-committed-read` and
  `local-write-first-read`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Page-index lookup returns `SCAN_REQUIRED` for incomplete-index states and
  page-present-but-too-new snapshot states.
- True empty index lookups still return `NOT_FOUND`.
- The database read path keeps the authoritative WAL scan for `NOT_FOUND`,
  `SCAN_REQUIRED`, and stale direct-index reads.
- DDL/CTAS ownerless coverage that failed under broad miss shortcuts passes.
- The embedded performance probe emits `page_read_index_scan_required`.

## Risks And Follow-Up

- Ownerless autocommit remains slower until a correctness-proven replacement
  for repeated negative WAL scans exists.
- Supporting older snapshot reads efficiently likely requires a historical page
  index or another bounded snapshot structure, not a latest-only index.
- SQL-level table-lock fault injection remains unproven because explored SQL
  shapes still stop before the ownerless table-wait callback.
