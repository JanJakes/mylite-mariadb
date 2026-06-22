# Ownerless Single-Owner Reclaim Budget Fast Reject

## Problem

The existing single-owner foreground reclaim budget keeps tight ownerless write
bursts from checkpointing page-version WAL in the caller's SQL step until the
WAL grows past `MYLITE_OWNERLESS_SINGLE_OWNER_FOREGROUND_RECLAIM_MIN_BYTES`, or
until a native file-operation checkpoint marker requires immediate cleanup.

Reduced production attribution after the 65536-row visible-fast batch slice
still showed `mylite_perf_ownerless_insert_autocommit_prepared_step_reclaim_ms`
at `59.189 ms` across 200 single-row ownerless autocommit inserts. Initial
inspection showed two separate measurement problems:

- below the 64 MiB single-owner foreground budget, a pure DML loop can still
  enter the active page-version pin snapshot before the budget check rejects
  reclaim;
- the production probe created and dropped its measured ownerless insert tables
  in the same open runtime immediately before timing, so delayed native
  file-operation checkpoint cleanup from benchmark setup could be charged to
  the DML loop.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc`
  `maybe_reclaim_ownerless_page_log_after_statement()` schedules statement
  reclaim only after successful ownerless writes, DDL, or explicit transaction
  end statements.
- `ownerless_statement_checkpoint_has_no_active_pins()` snapshots the shared
  page-version pin registry before statement reclaim can run.
- `ownerless-single-owner-foreground-reclaim-budget` already established that
  foreground statement reclaim can use a larger WAL budget while the runtime is
  still in its single-owner epoch and no native file-operation checkpoint
  marker is pending.
- `packages/libmylite/tests/embedded_performance_probe.c`
  `measure_transactional_insert()`, `measure_autocommit_insert()`, and
  `measure_bulk_autocommit_insert()` prepared their measured tables inside the
  same ownerless runtime as the timed DML loop.
- `reclaim_ownerless_page_log_after_native_checkpoint()` remains the only
  native checkpoint proof and page-log checkpointing boundary. This slice does
  not change that proof.

## Design

Move the existing single-owner foreground-budget rejection ahead of the active
pin snapshot in `maybe_reclaim_ownerless_page_log_after_statement()`.

The new helper
`ownerless_single_owner_foreground_reclaim_budget_skips()` preserves the
previous conditions:

- the runtime is still in the single-owner epoch,
- WAL payload bytes are below
  `MYLITE_OWNERLESS_SINGLE_OWNER_FOREGROUND_RECLAIM_MIN_BYTES`,
- the checkpoint file can be read,
- no native file-operation checkpoint marker is pending.

When those conditions hold, the function returns before snapshotting the pin
registry. When they do not hold, the function still takes the same active-pin,
timer-throttle, and native checkpoint proof path as before.

The embedded production performance probe also separates insert table setup
from ownerless insert timing:

- table setup remains inside the ordinary helper wrappers for existing call
  sites;
- ownerless insert timing prepares the transactional, single-row autocommit,
  and row-list autocommit tables first, then closes and reopens the ownerless
  runtime before enabling insert attribution and timing existing-table DML;
- the timed loops still use the same SQL shapes and summary keys.

## Non-Goals

- No WAL format changes.
- No page-version visibility or native checkpoint proof changes.
- No blind elision of history-proof native-support pages.
- No public C API, PHP API, or directory layout changes.
- No change to close-time or timer-driven reclaim scheduling.
- No change to SQL executed by the measured insert loops.

## Compatibility Impact

SQL behavior and MySQL/MariaDB compatibility are unchanged. The slice only
removes a redundant foreground reclaim eligibility check in the single-process
ownerless write-burst case that already skipped reclaim under the existing
budget, and adjusts a production probe so DML timing is not polluted by setup
DDL cleanup.

## Directory And Lifecycle Impact

No files or metadata are added. Retained WAL lifetime is unchanged: below the
single-owner foreground budget, WAL was already retained for timer or close
cleanup unless a native file-operation marker was pending. The probe's extra
ownerless close/reopen boundary is benchmark setup only; production API
behavior is unchanged.

## Native Storage Impact

Native InnoDB storage files, redo, checkpoint proof, page-version WAL replay,
and recovery anchors are unchanged. The native checkpoint reclaim function is
not modified.

## Build And Performance Impact

The hot path now performs the cheap single-owner budget rejection before the
active pin snapshot when the normal 64 KiB statement-reclaim threshold has
already been crossed. This is expected to reduce
`prepared_step_reclaim_ms` in production attribution probes for single-owner
autocommit insert loops below the 64 MiB foreground budget.

The production probe now reports ownerless insert DML after benchmark DDL setup
has been drained by close/reopen. This gives CI a clearer split between
steady-state DML and setup/checkpoint cleanup.

## Test And Verification Plan

- Build `mylite_embedded_performance_probe` and
  `mylite_ownerless_cross_process_sql_test` with the `php-embedded-prod`
  preset.
- Run a reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.
- Run focused production SQL coverage for
  `single-owner-foreground-reclaim-budget`,
  `single-owner-foreground-reclaim-peer-history`,
  `statement-checkpoint-scheduling`, `timer-checkpoint-scheduling`,
  `native-reclaim`, and `live-reclaim`.
- Run production ownerless primitive coverage.
- Run `format-check-prod` and `git diff --check`.

## Acceptance Criteria

- Single-owner foreground reclaim below the larger budget skips the active-pin
  snapshot when no native file-operation checkpoint marker is pending.
- Runtimes that have seen peers, pending native file-operation markers, WAL at
  or above the larger budget, active pins that matter for real reclaim, timer
  reclaim, and close-time reclaim keep the previous safety checks.
- Focused ownerless reclaim and primitive tests pass.
- Production attribution shows lower single-row autocommit reclaim timing
  without changing page-version or native-support publication semantics.
- The existing insert throughput summary names remain stable.

## Verification Results

Local verification on 2026-06-22:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  passed.
- The reduced stats-enabled production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` passed. Against the pre-slice
  reduced sample, ownerless single-row autocommit
  `prepared_step_reclaim_ms` moved from `59.189 ms` to `42.645 ms` while
  preserving `2.000` page-version records, `2.000` published native-support
  pages, and `1.000` history-proof rollback-segment plus `1.000`
  history-proof undo page per insert. The same reduced sample reported
  ownerless/ordinary row-list bulk throughput ratio `0.7765` after setup DDL
  drain, versus `0.4970` in the earlier same-shape sample.
- A reduced stats-off production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=2048`, and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=1024` passed. It reported
  ownerless single-row autocommit at `683.50 ops/s` versus `2906.34` ordinary
  (`0.2352` ratio), confirming that major native DML/write-publication work
  remains after the attribution cleanup.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-single-owner-foreground-reclaim-(budget|peer-history)$'
  --output-on-failure` passed.
- Direct production selectors passed:
  `statement-checkpoint-scheduling`, `timer-checkpoint-scheduling`,
  `native-reclaim`, and `live-reclaim`.
- `ctest --preset php-embedded-prod -L compat.ownerless-primitives
  --output-on-failure` passed.
- `tools/require-cmake-release-build build/php-embedded-prod` passed.
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Risks And Follow-Up

The helper intentionally preserves the existing marker predicate. Ownerless
`ALTER TABLE ... AUTO_INCREMENT` pending-bit scheduling is unchanged and remains
covered by the broader native checkpoint and close-time reclaim path.
