# Ownerless Page Read Refresh Diagnostics

## Problem

The WordPress PHPUnit split and focused comparisons show that ordinary PHP and
mysqli runtime is close to trunk. The remaining large performance gap is the
ownerless engine path, especially autocommit InnoDB inserts.

A stats-enabled embedded probe on 2026-06-08 at ownerless head `9089915b`
reported:

- ordinary autocommit inserts: `2100.82 ops/s`;
- ownerless transactional inserts: `366.09 ops/s`;
- ownerless autocommit inserts: `67.86 ops/s`;
- ownerless autocommit commit visibility: `200/200` visible-only, zero flush
  fallbacks;
- ownerless autocommit page publication: `1600/1600` published, zero skips or
  failures;
- measured ownerless autocommit hook buckets: page-write refresh `258.766ms`,
  page-version append `152.702ms`, page-index publish `8.649ms`,
  pages-visible sync/checkpoint `18.538ms`, and redo/table-lock hooks far below
  the full `2.947s` loop time.

`strace` is too intrusive for throughput, but it usefully amplified the shape
of the hidden cost. A 50-insert main-thread syscall aggregate spent visible
time in `fcntl`, `fstat`, `read`, and `pread64`, while `fdatasync` was small.
That points at repeated page-refresh reads and page-log scans rather than the
commit-visible sync path.

The previous broad page-index miss fast path was rejected because it broke
DDL/CTAS ownerless coverage. The next safe step is to expose where page reads
come from before changing refresh semantics.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` calls
  `ownerless_page_write_refresh()` before writing pages that need current
  ownerless visibility. SQL autocommit releases page-write locks at MTR
  boundaries so MTR page-version publication remains the proof path.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  implements `mylite_ownerless_innodb_refresh_page_for_write()` by observing
  ownerless redo state and then calling `refresh_page_for_write()`.
- `refresh_page_for_write()` first calls
  `mylite_ownerless_innodb_read_page_version()`. If no page-version record is
  available, it reads the native page from the tablespace and copies it only
  when the native page LSN is newer than the local page.
- `packages/libmylite/src/database.cc:ownerless_innodb_page_read_locked()`
  checks the shared page index first. If the page index misses or the indexed
  record is stale after checkpoint movement, it scans `mylite-concurrency.wal`
  through `mylite_ownerless_page_log_find_latest_at()`.
- `packages/libmylite/tests/embedded_performance_probe.c` already has opt-in
  ownerless performance buckets for page publication, pages-visible sync,
  logical locks, redo hooks, and page-write refresh. It does not yet distinguish
  page-index hits from WAL scans during page reads.

## Design

Extend the existing opt-in database performance counters with page-read
diagnostics:

- page read hook calls and total time;
- page-index lookup time;
- page-index direct hits;
- page-index misses;
- stale indexed records that fall back to a WAL scan;
- page-index errors;
- WAL scan calls and scan time;
- WAL scan found, not-found, and error outcomes.

Emit the new counters from `mylite_embedded_performance_probe` only when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, matching the existing diagnostic
gate. Default runtime behavior and default probe output stay unchanged.

## Compatibility Impact

No SQL behavior, public C API behavior, storage format, PHP API behavior, or
ownerless concurrency semantics change. This slice adds opt-in diagnostics only.

## Directory And Lifecycle Impact

No durable files or directory-layout changes. The counters are process-local
and reset by the performance probe around measured ownerless write loops.

## Native Storage Impact

No native storage behavior change. The diagnostics observe page-index lookups,
page-log direct reads, and page-log scans already performed by the refresh path.

## Build And Performance Impact

Default behavior is unchanged. With diagnostics enabled, the page-read hook adds
relaxed atomic counters and timing calls around already-expensive page-index and
WAL-read operations. This diagnostic overhead is limited to performance-probe
runs that explicitly enable existing ownerless stats.

## Test Plan

- Build `mylite_embedded_performance_probe`.
- Run a reduced stats-enabled embedded performance probe and verify the new
  page-read counters appear for ownerless transactional and autocommit write
  loops.
- Run focused ownerless visibility selectors that use page-version reads:
  `prepared-committed-read` and `local-write-first-read`.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Stats-enabled probe output separates page-index hits, misses, stale reads,
  and WAL scans.
- Default probe output remains unchanged when ownerless stats are disabled.
- Existing ownerless visibility behavior still passes focused tests.
- The slice does not add a page-read fast path or weaken DDL/CTAS correctness.

## Verification Results

Local verification on 2026-06-08 used the `embedded-dev` build tree.

- `cmake --build --preset embedded-dev --target
  mylite_embedded_performance_probe`: passed.
- Default probe smoke run passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=5`, and
  `MYLITE_PERF_INSERT_ITERATIONS=5`. Output kept
  `mylite_perf_ownerless_page_publish_stats=0` and did not emit page-read
  diagnostic keys.
- Stats-enabled probe passed with
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`,
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=200`, and
  `MYLITE_PERF_INSERT_ITERATIONS=50`.
- The stats-enabled run reported ownerless autocommit inserts at
  `123.08 ops/s`, `350/350` page-publish records, `50/50` visible-only commit
  decisions, `4046` page-read calls, `150` page-index hits, `3896` page-index
  misses, `3896` WAL scans, and `3896` WAL-scan misses. No stale index reads,
  page-index errors, WAL-scan hits, or WAL-scan errors were reported.
- The same run reported ownerless transactional inserts at `1024.44 ops/s`,
  `55/55` page-publish records, `28` page-read calls, `3` page-index hits,
  `25` page-index misses, `25` WAL scans, and `25` WAL-scan misses.
- Focused ownerless selectors passed:
  `prepared-committed-read` and `local-write-first-read`.

## Risks And Follow-Up

- Diagnostic counters add measurement overhead when enabled; throughput samples
  should be compared only with the same diagnostic mode.
- If page-index misses dominate simple DML refreshes, the follow-up optimization
  still needs a correctness proof narrower than the rejected broad index-miss
  shortcut.
