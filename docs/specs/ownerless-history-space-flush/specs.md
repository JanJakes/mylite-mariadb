# Ownerless History Space Flush

## Problem

The production ownerless autocommit attribution probe shows that the remaining
simple-insert write gap is dominated by
`trx_t::write_serialisation_history()`. The largest subphase is the ownerless
dirty-page flush immediately after the rollback-segment history mini-transaction
commits. That flush currently waits globally through the history MTR LSN, so it
can force unrelated data and index pages that are already covered by the
ownerless page-version WAL fast path to become native-durable before the commit
can continue.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` acquires an ownerless page-write lock
  on the rollback-segment history page, mutates the undo history list, commits
  the history MTR, then calls
  `mylite_ownerless_innodb_flush_dirty_pages_for_page_writes()` before
  releasing the history page-write lock.
- `mylite_ownerless_innodb_flush_dirty_pages_for_page_writes()` currently calls
  MariaDB `buf_flush_wait_flushed()`, whose source comment says it waits until
  all persistent dirty pages are flushed up to a limit.
- `buf_flush_list_space()` can issue flushes for one tablespace, but it does
  not by itself wait for a target LSN. A bounded ownerless optimization needs a
  target-LSN wait for one tablespace, not the existing best-effort flush loop.
- The later ownerless commit-visible fast path already avoids a global native
  dirty-page wait for proven one-row autocommit inserts. The write-history
  flush happens before that fast path and can therefore erase much of the fast
  path's intended benefit.

## Design

Add a MariaDB buffer-pool helper that waits until dirty pages in one tablespace
are flushed through a target LSN:

- scan `buf_pool.flush_list` under `buf_pool.flush_list_mutex` for the oldest
  dirty page in the requested tablespace,
- use the existing `buf_flush_list_space()` dispatcher to flush that space,
- wait for pending writes between attempts,
- keep the existing redo write ordering by ensuring the target LSN reaches the
  flushed redo position after the space-local page wait completes.

Add a MyLite ownerless hook wrapper for the new helper and call it from
`trx_t::write_serialisation_history()` with `rseg->space->id`. The ownerless
history-page handoff still requires native disk proof for the rollback segment
space before releasing the page-write lock. It no longer requires unrelated
user-table dirty pages at older LSNs to flush at that same boundary.

The existing global flush helper remains unchanged for broader commit fallback,
read-refresh, recovery, and any path whose correctness still requires a global
LSN boundary.

## Compatibility Impact

No SQL, C API, PHP API, directory layout, or native file format change. The
ownerless history-page handoff remains conservative for the rollback-segment
space, preserving cross-process writer serialization. User-table visibility
continues to rely on the existing page-version WAL proof and conservative
global fallback outside the already-proven fast path.

## Native Storage Impact

Rollback-segment history pages in the target tablespace remain native-flushed
before the ownerless history page-write lock is released. Other dirty pages may
remain dirty if they are outside the rollback-segment space and are covered by
the later ownerless page-version/visibility policy.

## Binary Size Impact

No new dependency or storage artifact. The embedded MariaDB archive gains one
small buffer-pool helper and one ownerless hook wrapper.

## Test Plan

- Rebuild the MariaDB embedded archive and production embedded performance
  probe.
- Run a reduced production stats-enabled embedded performance probe and compare
  ownerless write-history flush cost and autocommit throughput.
- Run focused production ownerless commit visibility coverage.
- Run hook crash coverage for visible publication/checkpoint boundaries if the
  focused production selector passes.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- The write-history ownerless flush uses a rollback-segment-space target-LSN
  wait instead of a global dirty-page wait.
- The broader ownerless global dirty-page flush helper remains available and
  unchanged for non-history paths.
- Focused ownerless SQL commit/visibility coverage passes.
- Performance probe output still reports the write-history subphase keys, with
  enough evidence to compare the targeted flush against the previous global
  wait.

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
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. It reported ownerless
  autocommit at `252.65 ops/s`, write history at `1.732 ms/insert`, and the
  rollback-segment-space dirty-page flush subphase at `1.014 ms/insert`.
- A stats-off production throughput probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000`. It reported ordinary autocommit at
  `1832.28 ops/s`, ownerless autocommit at `407.86 ops/s`, and an ownerless
  ratio of `0.2226`.
- Focused production SQL selectors passed:
  `commit-race`, `prepared-committed-read`, and `local-write-first-read`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed.
- Hook crash selectors passed:
  `visible-publish-crash` and `visible-checkpoint-crash`.
- The production ownerless primitive/hook CTest subset passed:
  `ctest --preset php-embedded-prod -R
  'libmylite\.(ownerless-primitives|embedded-ownerless-(mdl-hooks|trx-hooks|innodb-lock-hooks)|ownerless-single-owner-(page-write-refresh-skip|external-refresh-skip))$'
  --output-on-failure`.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Follow-Up

- This does not remove the native proof for undo/history pages. Skipping that
  proof would require a separate page-version correctness argument for dirty
  local rollback-segment pages across repeated peer handoffs.
- The benefit depends on how much unrelated user-table dirty-page work the
  global wait was forcing in the measured workload.
