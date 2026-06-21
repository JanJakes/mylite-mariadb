# Ownerless 48 Range Redo Batch

## Problem

The current ownerless visible-fast redo path batches native redo writes and
shared redo-state completion, but the deferred range buffer still flushes every
`32` ranges. The reduced 16384-row production probe after the larger visible
fast row-list slice reported two bulk statements with `32782.000` logical
page-write redo written/leave events on the later non-empty-table statement,
`1028` database-level redo leave callbacks across the two statements, and
`2.957 ms` in those database-level redo leave callbacks.

That path is no longer the dominant ownerless bulk cost, but it is still a
bounded callback/latch overhead that grows with large row-list statements.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_redo_leave()` defers top-level visible-fast
  `(start_lsn, end_lsn, latest_lsn)` completion through
  `mylite_ownerless_innodb_redo_defer_written_and_leave()` before falling back
  to immediate `log_write_up_to()` and fused written/leave completion.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  stores deferred ranges in a fixed thread-local array and flushes through the
  optional batch written/leave hook when more than one range is pending.
- `packages/libmylite/src/ownerless_redo_state.cc` has `64` shared active
  reservation slots. One slot is normally the active owner entry, so the
  deferred batch must remain comfortably below the reservation table capacity.
- `packages/libmylite/src/database.cc`
  `ownerless_innodb_redo_written_leave_batch_hook()` already validates the
  incoming range count against `MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES`
  and completes the batch through
  `mylite_ownerless_redo_state_complete_write_and_leave_batch()`.

## Design

Raise `MYLITE_OWNERLESS_INNODB_REDO_BATCH_MAX_RANGES` from `32` to `48`.

The new cap keeps the same fixed thread-local storage model and remains below
the `64` shared active-reservation slots, leaving reservation headroom for the
active owner entry, the next range that triggers a full-batch flush, and peer
activity. No batching is admitted outside the existing visible-fast deferred
page-publish scope, and unsafe hook builds still bypass the deferred path.

## Compatibility Impact

No SQL behavior, public C API, PHP API, WAL format, native redo format,
checkpoint format, page-version record format, shared-memory format, or
directory layout changes. The slice changes only how many already-deferred
visible-fast ownerless redo ranges are completed per shared redo-state batch
callback.

## Native Storage Impact

Native InnoDB redo bytes and mini-transaction commit LSNs are unchanged. Native
redo is still written before page-visible publication, and the shared
ownerless redo reservations remain active until the deferred flush completes.

## Binary Size And Dependencies

No new dependencies. The fixed thread-local deferred range buffer grows by
sixteen small range records in the existing InnoDB hook translation unit.

## Test And Verification Plan

- Build `mylite_embedded_ownerless_innodb_lock_hooks_test`,
  `mylite_ownerless_cross_process_sql_test`, and
  `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run the embedded hook test to prove a full `48`-range deferred batch uses one
  batch callback and reports the full completed count.
- Run the focused visible-fast SQL selector to preserve visible publication,
  page-version WAL, native-support proof, and database-level redo callback
  reduction assertions.
- Run a reduced stats-enabled production probe with `16384` rows per statement
  and compare database-level redo written/leave callback counts against the
  previous `32`-range sample.
- Run adjacent production ownerless selectors, hook selectors, stress
  selectors, production build guards, format checks, and whitespace checks.

## Verification Results

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_ownerless_innodb_lock_hooks_test
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.embedded-ownerless-innodb-lock-hooks$' --output-on-failure`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --output-on-failure`
- `cmake --build --preset ownerless-test-hooks --target
  mylite_embedded_ownerless_innodb_lock_hooks_test
  mylite_ownerless_cross_process_sql_test`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.(embedded-ownerless-innodb-lock-hooks|ownerless-single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision))$'
  --output-on-failure`
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test`
- `ctest --preset ownerless-stress -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|cross-process-checksum-stress)$'
  --output-on-failure`
- `tools/check-ci-production-builds`

The accepted reduced production probe used:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=32768
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=16384
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

Compared with the prior `32`-range sample, the rerun reported:

- database redo written callbacks: `1029` to `686`;
- database redo leave callbacks: `1028` to `685`;
- database redo leave time: `2.957 ms` to `2.618 ms`;
- page-write redo hook time: `6.060` to `5.703 ms/statement`;
- commit-log redo-leave time: `7.772` to `7.617 ms/statement`;
- logical page-write redo written/leave events unchanged at `16432.500` per
  statement;
- ownerless/ordinary bulk ratio: `0.6971` to `0.7717`;
- later-statement ownerless/ordinary bulk ratio: `0.4032` to `0.4406`.

An earlier rerun in the same local environment had worse overall
`mysql_query()` timing while preserving the callback reduction, so this slice
records the subphase and ratio improvement rather than claiming broad
throughput completion.

## Acceptance Criteria

- A full deferred batch carries `48` ranges to the batch callback.
- Database-level redo written/leave callback count drops for the same 16384-row
  stats-enabled probe while logical page-write redo written/leave event volume
  remains stable.
- Visible-fast SQL coverage still proves page-visible publication happens only
  after deferred redo completion.
- The cap remains below the shared active-reservation slot count.

## Risks

Larger batches keep ownerless redo reservations active for longer inside a
visible-fast statement. The cap is intentionally below the `64` reservation
slots and remains scoped to the already-proven statement boundary.
