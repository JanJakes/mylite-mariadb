# Ownerless Page-Log Negative Cache

## Problem

The WordPress PHPUnit CI steps are split from build and dependency setup, and
focused PHP timings stay close to trunk. The remaining measured performance gap
is in native ownerless autocommit writes.

A stats-enabled embedded probe on 2026-06-08 at `ce7da23b` reported:

- ordinary autocommit inserts: `1600.08 ops/s`;
- ownerless autocommit inserts: `89.34 ops/s`;
- ownerless autocommit visible-only commits: `199/200`;
- ownerless autocommit page reads: `16546`;
- page-index hits: `600`;
- true page-index misses: `15946`;
- page-index scan-required states: `0`;
- WAL scans: `15946`;
- WAL-scan misses: `15946`;
- WAL-scan time: `271.934ms`.

The previous page-index slice proved that skipping directly from index absence
is unsafe for CTAS and broader DDL guard selectors. The next bounded
performance slice therefore avoids a speculative skip: it can only reuse a
negative result after the authoritative page-version WAL scan has already
proved absence for the same page and page-index generation.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:ownerless_innodb_page_read_locked()`
  first asks the shared page-version index for a latest record offset, then
  calls `mylite_ownerless_page_log_find_latest_at()` when the index cannot
  prove the requested page version.
- `packages/libmylite/src/ownerless_page_log.cc:find_latest_in_snapshot()`
  scans every complete record from the page-log payload start to the selected
  snapshot end, tracking the best matching `(space_id, page_no)` record with
  `commit_lsn <= max_commit_lsn`. A miss proves absence only for that
  scanned snapshot and `max_commit_lsn`.
- `packages/libmylite/src/ownerless_page_index.cc` increments the index header
  generation whenever entries are published, cleared, replaced, or the index is
  marked as requiring WAL scan. That generation is a compact invalidation token
  for process-local negative proofs.

## Design

Add an internal page-index lookup variant that reports the index generation
observed under the page-index latch, plus an atomic current-generation read
helper for cache-hit validation.

Add a fixed-size process-local negative cache to the ownerless InnoDB hook
context. Each entry records:

- `space_id` and `page_no`;
- the page-index generation observed for the lookup;
- the largest `max_commit_lsn` for which a WAL scan has proven no record.

The read path uses the cache only when all of the following are true:

- the current page-index lookup returned `MYLITE_OWNERLESS_PAGE_INDEX_NOT_FOUND`;
- the cache entry matches the same page and page-index generation;
- a current page-index generation read still matches the generation observed
  by the `NOT_FOUND` lookup;
- the cached proven `max_commit_lsn` is greater than or equal to the requested
  `max_commit_lsn`.

The cache is populated only after
`mylite_ownerless_page_log_find_latest_at()` returns
`MYLITE_OWNERLESS_PAGE_LOG_NOT_FOUND` for an index `NOT_FOUND` state. Stale
index reads, `SCAN_REQUIRED`, errors, and found page-version records do not
populate it.

This is deliberately weaker than a true negative page-index proof. Any page-log
append that becomes part of the latest-page index, and any page-index clear,
replacement, or WAL-scan-required transition, changes the index generation and
invalidates the cached miss. A completed page-version append is ordered before
page-visible publication through the ownerless publish path, so a
same-generation cache hit cannot hide a committed visible page-version record.
The cache lives in process memory and is discarded on runtime close.

## Compatibility Impact

No SQL, public C API, PHP API, storage format, or durable directory layout
changes. Page-version read semantics remain conservative: the first miss for a
page/index-generation state still uses the authoritative WAL scan, and cache
hits only reuse that generation-bound negative proof.

## Directory And Lifecycle Impact

No new files and no directory-layout changes. Cache entries are runtime-local
and vanish on close, crash, process death, or hook reset.

## Native Storage Impact

No native storage format changes. Missing page versions still fall through to
the native InnoDB page when no page-version WAL image is available.

## Build And Performance Impact

The hot path adds a fixed-size in-memory cache lookup on page-index true
misses. Successful hits avoid repeated O(retained WAL records) negative scans
for the same page while the page-index generation remains unchanged. The
embedded performance probe reports cache hits and stores so future regressions
distinguish index misses, cache reuse, and real WAL scans.

Measured evidence on 2026-06-08 after the generation-bound cache showed the
reduced 200-insert probe still performing `16546` ownerless autocommit page
reads, but WAL-scan calls fell from `15946` to `5776` with `10170` negative
cache hits and `5776` stores. Page-read total time fell from the prior
`429.455ms` evidence to `233.701ms` in the final probe. End-to-end ownerless
autocommit throughput remained in the noisy `50-100 ops/s` band because
page-write refresh/publication and page-version WAL append still dominate the
short reduced probe.

## Test Plan

- Add primitive coverage for the page-index generation returned by lookup.
- Build `mylite_embedded_performance_probe`,
  `mylite_ownerless_primitives_test`, and
  `mylite_ownerless_cross_process_sql_test`.
- Run `mylite_ownerless_primitives_test`.
- Run a stats-enabled embedded performance probe and record negative-cache
  hits/stores plus WAL-scan counts.
- Run focused CTAS/DDL selectors that rejected direct index-miss skipping:
  `ctas-post-create-dml`, `ddl-broader`, and `online-ddl-options`.
- Run focused page-version selectors: `prepared-committed-read` and
  `local-write-first-read`.
- Run hook crash selectors: `visible-publish-crash` and
  `visible-checkpoint-crash`.
- Run ownerless SQL `stress`, `format-check`, and `git diff --check`.

## Acceptance Criteria

- A page-index lookup can report the generation observed under the latch.
- Cache-hit validation re-reads the current page-index generation and rejects
  stale proofs when a concurrent publish/rebuild advanced it.
- The negative cache is populated only after an authoritative WAL scan miss.
- Cache hits bypass the repeated WAL scan only for the same page, page-index
  generation, and a covered `max_commit_lsn`.
- The embedded performance probe emits cache hit/store counters and shows fewer
  WAL scans on the reduced ownerless autocommit workload.
- CTAS, broader DDL, online DDL, focused visibility, and crash selectors still
  pass.

## Risks And Follow-Up

- This cache reduces repeated misses only while the page-index generation is
  stable. It does not replace the latest-page index with a historical index.
- If later profiling shows cache invalidation is too broad, a separate slice
  should add a shared historical page-index primitive rather than weakening the
  read proof.
- Broader native redo/checkpoint reconciliation, DDL/file lifecycle recovery,
  SQL-level table-lock fault injection, and external randomized MariaDB/RQG
  stress remain separate ownerless completion work.
