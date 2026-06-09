# Ownerless Single-Owner Foreground Reclaim Budget

## Problem

Production-built performance probes show that prior ownerless hot-path slices
removed most statement-boundary refresh work from the single-owner autocommit
insert path, but tight autocommit loops can still run page-log reclamation in
the foreground every scheduler interval after the 64 KiB WAL threshold is
crossed.

Reduced production samples on 2026-06-08 reported ownerless autocommit insert
throughput between roughly `106` and `181 ops/s` for 80 inserts. The remaining
foreground cost was not page-write refresh, which stayed below 1 ms in the
same samples, but `prepared_step_reclaim_ms` varied from about `26 ms` to
`69 ms`. Page-version append/publish remained a separate cost at about
`23-27 ms` for the same sample.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `maybe_reclaim_ownerless_page_log_after_statement()` schedules foreground
  reclaim after successful ownerless write, DDL, or transaction-ending
  statements once `mylite-concurrency.wal` crosses
  `MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_MIN_BYTES`.
- `ownerless_checkpoint_scheduler_loop()` independently wakes every
  `MYLITE_OWNERLESS_PAGE_LOG_CHECKPOINT_INTERVAL_MS` while the ownerless
  runtime is idle and reuses the same native checkpoint reclaim path.
- `reclaim_ownerless_page_log_after_native_checkpoint()` remains the
  correctness boundary for native checkpoint proof, live-peer statement gates,
  active page-version pin retention, native write/recovery idleness, page-index
  replacement, and native file-operation checkpoint markers.
- The single-owner epoch proof is already used by
  `ownerless-single-owner-refresh-fast-path`: process-registry active count is
  exactly one and the registry generation still equals this runtime's slot
  generation. Once a peer joins or exits, the proof is permanently false for
  the runtime.

## Design

Keep the existing 64 KiB checkpoint threshold for:

- timer-driven reclaim,
- close-time reclaim,
- statement-boundary reclaim after the runtime has seen a peer,
- statement-boundary reclaim when a native DDL/file-operation checkpoint marker
  is pending.

For foreground statement-boundary reclaim only, when the runtime is still in
the single-owner epoch and no native file-operation checkpoint marker is
pending, require a larger WAL budget before reclaim runs in the caller's SQL
step. The default budget is 64 MiB through the internal
`MYLITE_OWNERLESS_SINGLE_OWNER_FOREGROUND_RECLAIM_MIN_BYTES` compile-time
constant.

The single-owner proof remains stricter than the current active-process count.
A runtime that has seen a real peer process join and exit must no longer use
the larger foreground budget even when the registry active count returns to
one. Focused coverage forks the peer child before the parent opens the writer
runtime so the child does not inherit the parent's embedded runtime state, then
opens the peer after the writer is registered and verifies the next thresholded
write reclaims synchronously.

This is a scheduling change, not a new checkpoint proof. The same timer and
close paths can still reclaim below the foreground budget, and the same
`reclaim_ownerless_page_log_after_native_checkpoint()` function decides whether
checkpointing is actually safe.
When that reclaim path compacts retained page-version WAL, it must first
publish the current buffer-pool page set to the reclaim LSN, wait for native
dirty pages to flush through that LSN, and then take the native checkpoint;
single-owner scheduling does not make retained ownerless boundary records
authoritative over native FK or DDL side-effect pages.

## Compatibility Impact

No SQL, public C API, PHP API, native storage format, or durable directory
layout changes. Foreground reclamation may be delayed for single-process
ownerless write bursts, but committed page versions remain durable in the WAL
and visible through existing ownerless page-version reads and no-live recovery.

## Directory And Lifecycle Impact

No files or metadata are added. A still-single-owner writer may retain more
page-version WAL before foreground cleanup. The retained WAL remains inside the
database directory and is eligible for timer-driven cleanup while idle and
close-time cleanup before runtime shutdown.

DDL/native file-operation checkpoint markers keep the prior foreground
behavior so file-lifecycle cleanup is not delayed behind the larger single
owner write budget.

## Native Storage Impact

Native InnoDB checkpoint proof is unchanged except for the retained-WAL flush
precondition above. The implementation only changes
when the caller attempts the existing reclaim path.

## Build And Performance Impact

The hot path adds one WAL payload-size calculation that replaces the previous
boolean threshold check, and one single-owner/native-file-marker branch only
after the normal 64 KiB threshold is crossed.

The measured effect is lower
`ownerless_insert_autocommit_prepared_step_reclaim_ms` in production probes for
single-process ownerless autocommit write bursts. It does not reduce
page-version append/payload-write volume or the broader InnoDB execute cost.

Production probe evidence on 2026-06-08:

- before this slice, reduced 80-insert samples reported ownerless autocommit
  at `106.88-180.97 ops/s` and foreground reclaim at `26.974-69.071 ms`;
- after this slice, reduced 80-insert samples reported ownerless autocommit at
  `165.37-194.32 ops/s` and foreground reclaim at `0.845-1.037 ms`;
- the default 200-insert production probe reported ordinary autocommit at
  `2078.44 ops/s`, ownerless transactional inserts at `1258.19 ops/s`,
  ownerless autocommit at `185.81 ops/s`, and ownerless autocommit foreground
  reclaim at `2.997 ms`.

The full probe still shows ownerless autocommit time in the InnoDB execute
path plus page-version publish/append volume: page-publish hook total
`66.892 ms`, append `59.127 ms`, page-write publish `74.925 ms`, and page-read
total `52.587 ms` for 200 ownerless autocommit inserts.

## Test Plan

- Build `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test` with the `php-embedded-prod`
  preset.
- Run the stats-enabled production performance probe and compare ownerless
  autocommit `prepared_step_reclaim_ms`, throughput, and page-log append
  counters.
- Add and run focused SQL coverage that keeps a same-process prepared result
  cursor active, performs a single-owner write burst, proves the WAL remains
  retained below the foreground budget, then finalizes the cursor and verifies
  timer cleanup.
- Add and run focused SQL coverage that registers a writer, has a cleanly
  forked peer process join and exit, waits past the foreground throttle
  interval before generating WAL, and verifies the next thresholded write
  checkpoints synchronously rather than using the larger budget.
- Run focused statement/timer/native reclaim selectors:
  `single-owner-foreground-reclaim-budget`,
  `single-owner-foreground-reclaim-peer-history`,
  `statement-checkpoint-scheduling`, `timer-checkpoint-scheduling`,
  `native-reclaim`, and `live-reclaim`.
- Run focused active-reader and DDL guard selectors:
  `active-reader-pressure`, `ctas-post-create-dml`, `ddl-broader`, and
  `online-ddl-options`.
- Run production ownerless primitive CTests.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- Single-owner foreground statement reclaim is skipped below the larger budget
  when no native file-operation checkpoint marker is pending.
- Timer-driven and close-time reclaim still checkpoint retained WAL below that
  budget when their existing safety predicates pass.
- Runtimes that have seen a peer keep the existing foreground scheduling
  threshold.
- Active page-version pins still retain WAL until release.
- Focused production ownerless correctness tests pass.

## Verification Results

Completed on 2026-06-08:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test
  mylite_ownerless_primitives_test` passed.
- Two reduced stats-enabled production probes with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`, `MYLITE_PERF_SELECT_ITERATIONS=50`,
  `MYLITE_PERF_INSERT_ITERATIONS=80`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed and reported the reduced
  metrics above.
- The default stats-enabled production performance probe passed and reported
  the 200-insert metrics above.
- Focused production ownerless selectors passed:
  `statement-checkpoint-scheduling`, `timer-checkpoint-scheduling`,
  `native-reclaim`, `live-reclaim`, `active-reader-pressure`,
  `ctas-post-create-dml`, `ddl-broader`, and `online-ddl-options`.
- `ctest --preset php-embedded-prod -L compat.ownerless-primitives
  --output-on-failure` passed, 2/2 tests.
- Added focused selector
  `single-owner-foreground-reclaim-budget` and CTest
  `libmylite.ownerless-single-owner-foreground-reclaim-budget`.
- Added focused selector
  `single-owner-foreground-reclaim-peer-history` and CTest
  `libmylite.ownerless-single-owner-foreground-reclaim-peer-history`.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-foreground-reclaim-budget` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-foreground-reclaim-peer-history` passed after correcting the
  test to fork the peer before the parent opened the writer runtime.
- `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-single-owner-foreground-reclaim-(budget|peer-history)$'
  --output-on-failure` passed, 2/2 tests.
- Adjacent production selectors passed after the focused test was added:
  `statement-checkpoint-scheduling`, `timer-checkpoint-scheduling`,
  `native-reclaim`, and `live-reclaim`.
- `ctest --preset php-embedded-prod -LE compat.ownerless-cross-process-sql
  --parallel 2 --output-on-failure` passed, 48/48 tests in 50.46 seconds.

## Risks And Follow-Up

- This intentionally trades larger short-lived single-owner WAL retention for
  lower foreground write latency. If long single-owner write bursts need a
  stricter disk-space cap, a follow-up can make the foreground budget
  configurable through the ownerless pressure policy.
- Page-version append volume and payload writes remain separate performance
  targets.
