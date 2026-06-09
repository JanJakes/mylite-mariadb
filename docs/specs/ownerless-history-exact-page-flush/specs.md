# Ownerless History Exact Page Flush

## Problem

Ownerless commits currently preserve native InnoDB rollback/undo recovery proof
by waiting for every dirty page in the rollback-segment tablespace to be clean
past the commit mini-transaction LSN. That is conservative and correct, but the
production attribution probe shows ownerless autocommit insert cost is now
concentrated in write-history flushing after native-support page WAL elision.

For the common one-row autocommit insert path, MariaDB's commit history update
has an exact page set: the rollback-segment header page and the undo log header
page. MyLite can try those pages first and keep the current tablespace-wide
wait as the fallback proof when the exact set is incomplete, not resident, not
flushable, or racing with other dirty pages.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` acquires the MyLite ownerless page
  write lock on `(rseg->space->id, rseg->page_no)`, mutates history state
  through `trx_purge_add_undo_to_history()`, commits the mini-transaction, and
  currently calls
  `mylite_ownerless_innodb_flush_space_dirty_pages_to_lsn(rseg->space->id,
  mtr->commit_lsn() + 1)` before releasing the page write lock.
- `mariadb/storage/innobase/trx/trx0purge.cc`
  `trx_purge_add_undo_to_history()` obtains the rollback segment header with
  `rseg->get(mtr, nullptr)` and the undo page with
  `buf_page_get(page_id_t(rseg->space->id, undo->hdr_page_no), ...,
  RW_X_LATCH, mtr)`, then updates the rollback segment history list and undo
  header state. The function nulls the `undo` reference, so the caller must
  capture `undo->hdr_page_no` before invoking it.
- `mariadb/storage/innobase/buf/buf0flu.cc`
  `buf_flush_wait_space_flushed()` checks the oldest dirty page in the target
  tablespace and then repeatedly calls `buf_flush_list_space()`, waits for
  pending writes, and rechecks the oldest dirty LSN until the tablespace is
  clean past `sync_lsn`.
- `buf_flush_list_space()` scans `buf_pool.flush_list` and flushes every dirty
  page in the space. It already profiles ownerless flushed page types and page
  identities when deep stats are enabled.
- `buf_page_t::flush(fil_space_t*)` can be used when the caller holds
  `buf_pool.mutex`, does not hold `buf_pool.flush_list_mutex`, and holds the
  page U-lock. It queues asynchronous page writes and releases
  `buf_pool.mutex` when a write is initiated.

## Design

Add a MyLite-owned history flush wrapper that accepts the rollback-segment
space id, the rollback-segment header page number, the undo header page number,
and the target LSN.

The buffer flush implementation will:

- check whether the target tablespace has dirty pages older than `sync_lsn`;
- when it does, enable the existing ownerless flush page-type profiler;
- try to flush the two known history pages directly by looking them up in the
  buffer-pool page hash, taking the page U-lock, and calling
  `buf_page_t::flush()`;
- wait for the direct writes and flush buffered doublewrite writes;
- re-run the existing tablespace-oldest check;
- call the current space-wide `buf_flush_list_space()` loop if any older dirty
  page remains.

The fallback is part of the design, not a debug-only escape hatch. A direct
flush is an optimization attempt only; correctness remains the existing
tablespace-wide proof when the exact pages do not fully cover the native
history boundary.

Capture the undo header page number in `write_serialisation_history()` before
`trx_purge_add_undo_to_history()` nulls `undo`, and pass it to the new wrapper
after `mtr->commit()`.

Add deep performance counters for exact history pages flushed and fallback
rounds so CI attribution can prove whether the exact prefix is being used.
Keep the existing total ownerless history flush page counter as the durable
proof counter.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, metadata, or wire-protocol behavior changes.
The commit path still preserves MariaDB native InnoDB recovery semantics for
ownerless writes by requiring the rollback/undo history boundary to be durable
before releasing the ownerless history page write lock.

## Directory And Lifecycle Impact

No durable directory-layout changes. The flush still writes MariaDB native
pages inside the MyLite database directory and still falls back to the existing
InnoDB tablespace flush path.

## Native Storage Impact

The native InnoDB page format is unchanged. The direct flush path queues normal
InnoDB page writes for pages that are already dirty in the buffer pool. It does
not publish ownerless page-log records for undo pages and does not reduce the
required native history-space proof.

## Build And Performance Impact

The common ownerless autocommit case can avoid a full rollback-segment
tablespace flush-list scan when the rollback-segment header and undo header
pages are the only dirty pages below the commit LSN. The optimization can still
perform native I/O for those pages, so it is expected to reduce scan/fallback
overhead rather than remove the native durability cost.

The new code is compiled into the MariaDB embedded archive and uses no new
dependencies.

## Test And Verification Plan

- Build the MariaDB embedded archive and production MyLite embedded targets.
- Run a focused ownerless test that asserts history exact-flush counters are
  exercised, fallback counts do not exceed exact attempts for the simple
  single-owner insert case, rows survive ownerless reopen, and rows survive a
  forced shared-memory rebuild.
- Run the existing native history flush proof and native-support WAL elision
  selectors.
- Run a reduced production attribution probe and confirm the new
  `mylite_perf_summary_*_exact_flush_*` keys are present.
- Run focused ownerless primitive and hook/stress coverage affected by the
  commit path.
- Run `cmake --build --preset format-check-prod`.
- Run `git diff --check`.

## Verification Results

Local verification on 2026-06-09 used production artifacts:
`build/mariadb-embedded` was `MinSizeRel`, `build/php-embedded-prod` was
`Release`, and `build/ownerless-test-hooks` was `Release`.

- `tools/mariadb-embedded-build build` passed and rebuilt
  `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-single-owner-history-flush-native-proof$'
  --output-on-failure` passed, including the new exact-flush counter
  assertions and reopen/shared-memory rebuild durability checks.
- A reduced production attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed. It reported ownerless
  autocommit history flushing at `0.669 ms/insert`, `2.000` total history flush
  pages per insert, `2.000` exact history flush pages per insert, and `0.000`
  exact-flush fallback rounds per insert. The flushed page mix remained one
  undo log page and one rollback-segment system page per insert.
- Final post-relink attribution samples under current local load continued to
  report `2.000` exact history flush pages per insert, `0.000` exact-flush
  fallback rounds per insert, and the same one undo log plus one
  rollback-segment system page mix. Their ownerless history flush timing was
  noisier, at `2.566 ms/insert` and `1.297 ms/insert`, with ownerless
  autocommit throughput of `183.69` and `326.70 ops/s`.
- Three stats-off 2000-row production samples reported ownerless autocommit
  insert throughput of `419.55`, `350.36`, and `391.25 ops/s`. These samples do
  not prove a broad throughput win, but they keep the branch in the previously
  observed noisy range while the attribution probe shows the exact prefix is
  active.
- Focused production CTest coverage passed:
  `ownerless-primitives`, `ownerless-single-owner-page-write-refresh-skip`,
  `ownerless-single-owner-external-refresh-skip`,
  `ownerless-single-owner-history-flush-native-proof`,
  `ownerless-single-owner-native-support-page-wal-elision`, and
  `ownerless-uncommitted-peer-hidden`.
- Direct production selectors passed: `commit-race`, `prepared-committed-read`,
  `local-write-first-read`, `live-reclaim`, and `stress`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed, and
  `tools/require-cmake-release-build build/ownerless-test-hooks` accepted the
  hook build cache.
- Hook crash selectors passed: `visible-publish-crash` and
  `visible-checkpoint-crash`.
- After formatting, `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed again, and the focused native history proof CTest passed again.
- After formatting, `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test` passed again,
  `tools/require-cmake-release-build build/ownerless-test-hooks` accepted the
  cache, and the hook crash selectors passed again.
- Production hook CTests passed:
  `embedded-ownerless-mdl-hooks`, `embedded-ownerless-trx-hooks`, and
  `embedded-ownerless-innodb-lock-hooks`.
- Production tool trace CTests passed:
  `tools.ownerless-independent-table-stress-trace`,
  `tools.ownerless-checksum-stress-trace`,
  `tools.ownerless-transaction-stress-trace`, and
  `tools.ownerless-temporary-table-stress-trace`.
- `tools/require-cmake-release-build build/prod build/php-embedded-prod
  build/ownerless-test-hooks` accepted all three Release caches.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` accepted
  the MariaDB embedded cache.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Ownerless history commit flushing first tries the exact rollback-segment and
  undo header pages.
- Any incomplete exact attempt falls back to the existing space-wide wait before
  the ownerless history page write lock is released.
- Existing total history flush page accounting remains correct.
- Deep stats expose exact history pages and fallback rounds.
- Focused correctness tests and production performance probes pass.

## Risks And Unresolved Questions

- Direct page flushing still writes native InnoDB pages; it cannot eliminate the
  write-history durability cost by itself.
- Workloads with additional dirty rollback-segment-space pages will still fall
  back to the current space-wide loop.
- Broader redo/checkpoint reconciliation remains a separate remaining
  ownerless concurrency gap.
