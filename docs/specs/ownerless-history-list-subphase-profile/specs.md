# Ownerless History List Subphase Profile

## Problem

Production ownerless autocommit probes now run against optimized CI-style
builds, and the native history WAL proof has removed the old exact dirty-page
flush from the hot path for single-row autocommit inserts. The remaining
`trx_commit_persist_write_history_history_list` bucket is still large enough
to hide several different costs: ownerless transaction-number assignment,
purge queue serialization, undo-list mutation, rollback-segment and undo page
refresh, and undo-history publication. The adjacent rollback-segment reference
release and latch unlock are also unmeasured today.

MyLite needs subphase attribution before choosing the next optimization. The
same code path protects InnoDB commit serialization and purge ordering, so a
speculative shortcut would be correctness-sensitive.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` times one broad
  `...WRITE_HISTORY_HISTORY_LIST_NS` interval around transaction-number
  assignment, purge queue enqueue, `UT_LIST_REMOVE(rseg->undo_list, undo)`,
  and `trx_purge_add_undo_to_history()`. The following `rseg->release()` and
  `rseg->latch.wr_unlock()` calls are outside that existing total but still
  run before the write-history mini-transaction is committed.
- `mariadb/storage/innobase/include/trx0sys.h`
  `trx_sys_t::assign_new_trx_no()` calls the MyLite ownerless transaction hook
  first when hooks are enabled, otherwise it uses the native transaction id
  counter and refreshes the read-write transaction hash version.
- `mariadb/storage/innobase/trx/trx0purge.cc`
  `trx_purge_add_undo_to_history()` fixes the rollback-segment and undo header
  pages, performs ownerless page refresh when hooks are active, decides whether
  the undo log can be cached, prepends the history list, and writes the final
  undo state and transaction number into the undo header page.
- `mariadb/storage/innobase/include/trx0rseg.h`
  `trx_rseg_t::release()` only decrements the rollback-segment reference
  count; any large cost here would be latch or atomic-contention evidence, not
  durable storage work.

## Design

Keep InnoDB control flow unchanged. Add deep performance counters for these
substeps inside `trx_t::write_serialisation_history()`:

- transaction-number assignment;
- purge queue lock/enqueue/unlock when a rollback segment becomes non-empty;
- rollback-segment undo-list removal;
- `trx_purge_add_undo_to_history()`;
- adjacent rollback-segment reference release;
- adjacent rollback-segment latch unlock.

Expose the counters through `mylite_embedded_performance_probe` as detailed
`mylite_perf_ownerless_insert_autocommit_innodb_deep_*` keys and compact
`mylite_perf_summary_ownerless_autocommit_write_history_*_ms_per_insert`
summary keys.

The counters use the existing opt-in deep performance flag. Stats-off
production probes keep the same hot-path behavior except for dead enum values
and one cached deep-stats flag check in `write_serialisation_history()`. The
new subcounter clock reads and atomic counter updates are skipped unless deep
stats are enabled.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, metadata, wire-protocol, directory layout, or
storage-format behavior changes. This slice is diagnostic instrumentation only.

## Directory And Lifecycle Impact

No new files are created in a MyLite database directory. No ownerless
visibility, page-version WAL, recovery, open/close, or cleanup behavior
changes.

## Native Storage Impact

No native InnoDB page, redo, undo, purge, checkpoint, or rollback-segment
semantics change. The instrumentation brackets existing operations without
changing lock ordering or recovery requirements.

## Build And Performance Impact

The MariaDB embedded archive must be rebuilt after changing InnoDB source.
Production timing evidence must use the CI-equivalent optimized artifacts:

- `build/mariadb-embedded` with `CMAKE_BUILD_TYPE=MinSizeRel`;
- `build/php-embedded-prod` with `CMAKE_BUILD_TYPE=Release`;
- production performance probes and focused ownerless SQL tests.

Stats-enabled attribution samples incur extra clock reads. Stats-off probes
remain the throughput signal.

## Test And Verification Plan

- Verify the local production CMake caches with
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` and
  `tools/require-cmake-release-build build/php-embedded-prod`.
- Rebuild the MariaDB embedded archive with `tools/mariadb-embedded-build
  build`.
- Build production `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test`.
- Run focused ownerless selectors for the native history WAL proof and commit
  race.
- Run a reduced stats-enabled production attribution probe and verify the new
  summary keys are present.
- Run a reduced stats-off production throughput probe to confirm the
  instrumentation slice did not obviously perturb disabled-counter throughput.
- Run `cmake --build --preset format-check-prod` and `git diff --check`.

## Acceptance Criteria

- The existing `trx_commit_persist_write_history_history_list` total remains
  available.
- New subphase keys identify the dominant part of that history-list interval.
- Focused production ownerless correctness coverage passes.
- No performance parity or broader ownerless concurrency-completion claim is
  added.

## Verification Results

Local verification on 2026-06-10 used production artifacts:
`build/mariadb-embedded` was `MinSizeRel` and `build/php-embedded-prod` was
`Release`.

- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` passed.
- `tools/require-cmake-release-build build/php-embedded-prod` passed.
- `tools/mariadb-embedded-build build` passed after the InnoDB source change
  and rebuilt `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-(primitives|single-owner-(history-wal-proof|native-support-page-wal-elision)|uncommitted-peer-hidden)$'
  --output-on-failure` passed after the final rebuild.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  commit-race` passed after the final rebuild.
- A reduced stats-enabled production attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` printed the new detailed and
  summary keys. The final sample reported ownerless autocommit at
  `606.52 ops/s`, `3.000` page-version records per insert, `3.570`
  native-support pages per insert, `1.570` native-support pages elided per
  insert, `0.176 ms/insert` in write history, `0.075 ms/insert` in the
  history-list interval, `0.073 ms/insert` in
  `history_list_purge_add_undo`, `0.002 ms/insert` in
  `history_list_assign_trx_no`, no measurable purge-queue, undo-list remove,
  adjacent rollback-segment release, or adjacent rollback-segment unlock time,
  `0.097 ms/insert` in write-history MTR commit, and zero ownerless history
  flush pages.
- A stats-off production throughput sample with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`, and
  `MYLITE_PERF_INSERT_ITERATIONS=1000` reported ordinary autocommit at
  `2218.09 ops/s`, ownerless autocommit at `552.17 ops/s`, ownerless
  autocommit ratio `0.2489`, ordinary transaction inserts at
  `2345.40 ops/s`, ownerless transaction inserts at `1461.53 ops/s`, and
  ownerless transaction ratio `0.6231`. Local production samples remained
  noisy, so this is a disabled-counter sanity check rather than a parity claim.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Unresolved Questions

- Subphase instrumentation can perturb stats-enabled timings. Use it for
  attribution only.
- The next optimization may still require broader native redo/checkpoint or
  purge-order proof before changing behavior.
- Broader DDL/file lifecycle recovery, external MariaDB/RQG stress, and
  SQL-level table-lock fault injection remain outside this slice.
