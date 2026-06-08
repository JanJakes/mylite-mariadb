# Ownerless Page-Log Read Guard Profile

## Problem

The WordPress PHPUnit CI job now separates Docker/source/build/dependency
setup, database preparation, a mysqli performance probe, and two PHPUnit
test-only suite steps. That made the remaining ownerless-specific runtime cost
visible: process startup is not the main gap, while ownerless autocommit writes
still spend substantial time in page-version read and refresh work.

A stats-enabled embedded probe on 2026-06-08 at `b5130c6a` reported:

- ordinary warm open/close: `430.414ms`;
- ownerless warm open/close: `450.823ms`;
- ordinary autocommit inserts: `1457.90 ops/s`;
- ownerless autocommit inserts: `58.94 ops/s`;
- ownerless autocommit page reads: `10111`;
- page-index direct reads: `600` taking `26.737ms`;
- page-index misses: `9511`;
- WAL scans: `5776` taking `92.727ms`;
- generation-bound negative-cache hits: `3735`;
- page-write refresh: `207.807ms`;
- page-version read detail: `3866` calls taking `139.084ms`;
- page-log append total: `104.818ms`.

The count profile shows that the next safe slice should reduce per-read
overhead without broadening the negative-cache proof that earlier CTAS and DDL
selectors rejected.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc:ownerless_innodb_page_read_hook()`
  already calls `mylite_ownerless_page_log_begin_read()` before consulting the
  page-version index or scanning the page-version WAL. That read guard prevents
  checkpoint truncation while an indexed record offset or WAL scan is in use.
- The helper used for direct index hits,
  `mylite_ownerless_page_log_read_page_at()`, took another checkpoint read lock
  internally before reading the already-indexed record offset.
- The helper used for index misses,
  `mylite_ownerless_page_log_find_latest_at()`, took an append-range snapshot
  lock to capture the log end, then took another checkpoint read lock before
  scanning that snapshot.
- The read hook therefore paid redundant checkpoint lock/unlock syscalls under
  an already-active read guard. Those syscalls do not change the page-version
  proof: the index lookup, direct record read, and WAL scan are already enclosed
  by the outer guard.

## Design

Add page-log helper variants for callers that already hold a page-log read
guard:

- `mylite_ownerless_page_log_read_page_under_read_lock_at()` validates and
  reads a page-identity-matched record offset without taking another checkpoint
  read lock.
- `mylite_ownerless_page_log_find_latest_under_read_lock_at()` takes only the
  append-range snapshot lock needed to choose a stable log end, then scans that
  snapshot under the caller's checkpoint read guard.

Switch the ownerless InnoDB page-read hook to these helpers. The hook still
acquires one page-log read guard before the page-index lookup and releases it
after the read path finishes. Negative-cache hit semantics are unchanged, and
true page-index misses still use the authoritative WAL scan before storing a
negative proof.

## Compatibility Impact

No SQL, public C API, PHP API, storage format, or durable directory layout
changes. The new helpers are internal ownerless page-log primitives used by the
embedded ownerless runtime.

## Directory And Lifecycle Impact

No new files and no directory-layout changes. Existing page-version WAL and
checkpoint locks keep their byte ranges and conflict rules.

## Native Storage Impact

No native storage format changes. Missing page versions still fall through to
MariaDB's native InnoDB page path, and page-version overlays still require the
same identity, LSN, and checksum checks.

## Build And Performance Impact

This slice reduces lock/syscall overhead for ownerless page-version reads. It
does not reduce page-read counts, WAL-scan counts, page-log append volume, or
page-write refresh counts.

Post-change probes were noisy. The first run improved direct indexed read time
from `26.737ms` to `9.888ms`, but had a slower WAL-scan sample and ownerless
autocommit dropped to `48.76 ops/s`. A second same-machine run reported:

- ordinary warm open/close: `478.546ms`;
- ownerless warm open/close: `408.971ms`;
- ordinary autocommit inserts: `2004.92 ops/s`;
- ownerless autocommit inserts: `87.19 ops/s`;
- ownerless autocommit page reads: `10111`;
- page-index direct reads: `600` taking `13.049ms`;
- page-index misses: `9511`;
- WAL scans: `5776` taking `71.325ms`;
- generation-bound negative-cache hits: `3735`;
- page-write refresh: `186.784ms`;
- page-version read detail: `3866` calls taking `87.162ms`;
- page-log append total: `70.378ms`.

The stable evidence is the unchanged count profile and lower direct-read cost.
The remaining ownerless autocommit gap is still dominated by repeated
page-version miss scans, page-write refresh disk/native checks, and page-log
payload appends.

## Test Plan

- Add primitive coverage for read-guard-aware direct record reads, latest-page
  scans, and not-found scans against an offset page log.
- Build `mylite_ownerless_primitives_test`,
  `mylite_embedded_performance_probe`, and
  `mylite_ownerless_cross_process_sql_test`.
- Run `mylite_ownerless_primitives_test`.
- Run a stats-enabled embedded performance probe and record startup, ordinary
  and ownerless SQL rates, page-read detail, page-write refresh detail, and
  append detail.
- Run focused ownerless selectors that previously rejected unsafe page-index
  shortcuts: `prepared-committed-read`, `local-write-first-read`,
  `ctas-post-create-dml`, `ddl-broader`, and `online-ddl-options`.
- Run `ctest --preset embedded-dev -L compat.ownerless-primitives`.
- Run `format-check` and `git diff --check`.

## Verification

Completed on 2026-06-08:

- `cmake --build --preset embedded-dev --target
  mylite_ownerless_primitives_test mylite_embedded_performance_probe
  mylite_ownerless_cross_process_sql_test` passed.
- `build/embedded-dev/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/embedded-dev/packages/libmylite/mylite_embedded_performance_probe`
  passed twice; the second sample is the performance evidence recorded above.
- Focused ownerless selectors passed: `prepared-committed-read`,
  `local-write-first-read`, `ctas-post-create-dml`, `ddl-broader`, and
  `online-ddl-options`.
- `ctest --preset embedded-dev -L compat.ownerless-primitives
  --output-on-failure` passed.
- `cmake --build --preset format-check` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Ownerless page reads retain one checkpoint read guard across page-index
  lookup and page-log access.
- Direct indexed record reads do not take a nested checkpoint read lock.
- WAL miss scans still snapshot the append end and then perform the
  authoritative scan under the existing checkpoint read guard.
- Negative-cache hit/store conditions are unchanged.
- Focused CTAS/DDL/page-version selectors and primitive tests pass.

## Risks And Follow-Up

- This is an overhead reduction, not a fix for the remaining scan count. The
  next performance slices should target repeated page-version miss proofs or
  page-write refresh disk/native checks without weakening the CTAS/DDL safety
  constraints.
- Broader native redo/checkpoint reconciliation, DDL/file lifecycle recovery,
  SQL-level table-lock fault injection, and external randomized MariaDB/RQG
  stress remain separate ownerless completion work.
