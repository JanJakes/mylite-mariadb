# Ownerless Native Support Page Elision

## Problem

Production ownerless autocommit profiling shows the simple one-row `INSERT`
path still writes four ownerless page-version WAL records per row. Three of
those records are native InnoDB support pages: undo-log pages and transaction
system pages. The same commit then enters
`trx_t::write_serialisation_history()` and flushes the rollback-segment
tablespace through the history mini-transaction LSN before ownerless visibility
is published.

That means the hot path duplicates support-page evidence: it writes full page
images to `concurrency/mylite-concurrency.wal`, then immediately proves the
same support state in native storage.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_page_write_publish()` publishes each modified MTR page
  image after `m_commit_lsn` is known. The stats-enabled production probe for
  200 one-row ownerless autocommit inserts reported `800` publish candidates:
  `200` index pages, `400` undo pages, and `200` transaction-system pages.
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` acquires the rollback-segment page
  write lock, commits the history mini-transaction, then calls
  `mylite_ownerless_innodb_flush_space_dirty_pages_to_lsn(rseg->space->id,
  mtr->commit_lsn() + 1)` before releasing the history page-write lock.
- `mariadb/storage/innobase/buf/buf0flu.cc`
  `buf_flush_wait_space_flushed()` waits until dirty pages in that tablespace
  are flushed through the requested LSN and profiles flushed page types for
  the ownerless attribution probe.
- The current production profile can use dedicated undo tablespaces. The
  mandatory history flush still covers the rollback-segment tablespace
  identified by `rseg->space->id`.
- Native-support pages outside `rseg->space->id` need a separate proof because
  the history flush does not cover their tablespace.

## Design

Add a narrow native-support elision gate in
`mtr_t::ownerless_page_write_publish()` after the source page LSN has been
verified against the MTR commit LSN, but before the page is copied,
checksummed, or passed to the ownerless page-version append hook. The function
skips the ownerless page-version WAL append only when all of the following are
true:

- the page type is one of the existing native-support page classes,
- the SQL shape is the already-proven one-row ownerless autocommit `INSERT`
  shape,
- the transaction has a redo rollback segment,
- the page itself is in `rseg->space->id`, and
- the transaction still publishes at least one non-native-support page image
  before it can use the visible-only commit path.

Every other page and statement class keeps the existing ownerless page-version
WAL publication path. Native-support pages outside the rollback-segment
tablespace therefore keep publishing page records in this slice.

The page-publish proof flag remains conservative: elided native-support pages
do not count as a published page image. They also do not count as a failure.
The visible-only commit fast path still requires at least one real page image
published to the ownerless page-version WAL and no publish failures.

Add a page-publish counter for native-support records elided by this gate.
The performance probe prints the raw counter and a per-insert summary so CI
attribution logs distinguish reduced WAL volume from missing page-publish
coverage.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, storage format, or directory layout
changes. Ownerless peer visibility remains anchored on the page-version WAL for
user/index pages and on the existing native history-space flush proof for
rollback-segment support pages.

## Native Storage Impact

No native format changes. The optimization relies on native support pages in
the rollback-segment tablespace being flushed through the history MTR LSN
before ownerless visibility advances. Native-support pages in other tablespaces
are left unchanged.

## Build And Performance Impact

For the default embedded ownerless layout, one-row autocommit inserts should
write about one ownerless page-version WAL record per row instead of four,
while retaining the mandatory native history flush. The slice does not remove
the history flush, so it targets page-log append and page-publish copy/checksum
cost, not rollback-segment flush cost.

## Test Plan

- Rebuild the MariaDB embedded archive and production embedded targets after
  editing InnoDB.
- Add a focused ownerless SQL test that enables page-publish stats, performs
  one-row autocommit inserts, verifies native-support elision, and then proves
  the rows survive ownerless reopen and
  forced shared-memory rebuild.
- Run a reduced stats-enabled production embedded performance probe and verify
  page-version records per insert fall while native-support elision per insert
  is reported.
- Run focused ownerless commit/read/reclaim selectors and the native history
  proof selector.
- Run hook crash-boundary selectors because page-version publication timing is
  changed.
- Run production build guards, `format-check-prod`, and `git diff --check`.

## Acceptance Criteria

- Ownerless inserts elide native-support page-version WAL records only in the
  rollback-segment tablespace and still publish at least one non-native page
  image.
- The commit fast path remains blocked if no real page image is published or a
  page-publish failure occurs.
- Native-support pages outside the rollback-segment tablespace do not use this
  elision path in this slice.
- Focused ownerless visibility, history-proof, reclaim, and crash-boundary
  tests pass.
- Production attribution logs expose both published page-version volume and
  native-support elision volume.

## Verification Results

Local verification on 2026-06-09 used production builds:

- `tools/mariadb-embedded-build build` rebuilt
  `build/mariadb-embedded` with `CMAKE_BUILD_TYPE=MinSizeRel`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  rebuilt the affected Release targets.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-single-owner-native-support-page-wal-elision$'
  --output-on-failure` passed.
- The reduced stats-enabled production probe
  (`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`) passed. Ownerless
  autocommit reported `800` page-publish candidates, `200` published
  page-version WAL records, `600` native-support page elisions, zero skips or
  failures, `199/200` visible-fast commits, `1.000` page-version WAL record
  per insert, and `3.000` native-support elisions per insert. The page-write
  publish bucket was `0.064 ms/insert`; write-history ownerless flush remained
  dominant at `1.005 ms/insert`.
- Three stats-off production throughput samples with `1000` insert iterations
  reported ownerless autocommit at `287.00`, `526.18`, and `443.62 ops/s`.
  The best sample is materially above the prior quiet local sample around
  `414 ops/s`, but the variance remains dominated by the native history flush.
- Focused production ownerless selectors passed: `commit-race`,
  `prepared-committed-read`, `local-write-first-read`, and `live-reclaim`.
- Focused production CTest coverage passed for single-owner page-write refresh,
  external refresh, native history flush proof, native-support page WAL
  elision, and uncommitted peer visibility.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed with
  `build/ownerless-test-hooks` confirmed as a Release build. Hook selectors
  `visible-publish-crash` and `visible-checkpoint-crash` passed.
- Direct ownerless independent-table stress passed through
  `mylite_ownerless_cross_process_sql_test stress`.
- Production trace CTests passed for independent-table, checksum,
  transaction, and temporary-table stress trace generators.
- The production ownerless primitive and embedded hook subset passed.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.

## Risks And Follow-Up

- This is not a broader redo/checkpoint reconciliation. It does not elide user
  table pages, dictionary pages, DDL/file-lifecycle pages, or native-support
  pages outside the rollback-segment tablespace.
- The mandatory history flush remains a large ownerless autocommit cost.
  Removing or batching that proof still requires separate peer handoff and
  recovery evidence.
