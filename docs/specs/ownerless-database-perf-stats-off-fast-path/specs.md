# Ownerless Database Perf Stats-Off Fast Path

## Problem

Normal production and CI timing runs keep first-party database and embedded-open
performance counters disabled. The elapsed-time helpers still had the same
avoidable stats-off shape that was previously fixed for ownerless page-write
timing: callers store `start_ns = 0` when stats are disabled, then the elapsed
helper calls back through the add helper, which reloads the disabled flag before
doing no work.

This is not the main ownerless write-throughput bottleneck. The current
stats-enabled probe still points at native row/undo mini-transaction work,
history-proof publication, page-version append work, and redo hook handoff.
The slice removes a small diagnostic overhead from normal stats-disabled
production paths so CI timing and WordPress PHPUnit process startup are not
paying avoidable perf-helper work.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/src/database.cc` owns the first-party database perf
  counters through `ownerless_database_perf_stats_enabled`,
  `ownerless_database_perf_add_elapsed()`, and
  `OwnerlessDatabasePerfScope`.
- The same file owns embedded open/close perf counters through
  `embedded_open_perf_stats_enabled`, `embedded_open_perf_add_elapsed()`, and
  `EmbeddedOpenPerfScope`.
- Both scope types already use `0` as the stats-disabled start-time sentinel.
  Returning immediately on that sentinel preserves semantics and avoids the
  extra disabled-flag load through the add helper.
- Existing ownerless SQL coverage has a native-support page-version selector
  that enables database perf counters for the write workload and already checks
  page-write perf counters stay zero when disabled.

## Scope And Non-Goals

In scope:

- first-party database elapsed perf helper;
- first-party embedded open elapsed perf helper;
- focused ownerless SQL coverage proving disabled database counters stay zero
  through an ownerless read;
- documentation and compatibility/performance notes.

Out of scope:

- MariaDB-derived page-write perf helpers, already covered by an earlier slice;
- changing perf counter names, indexes, or public test hook APIs;
- changing SQL behavior, storage formats, page-version WAL formats, checkpoint
  formats, or ownerless recovery semantics;
- solving the remaining native row/undo mini-transaction and redo handoff cost.

## Design

`ownerless_database_perf_add_elapsed()` and
`embedded_open_perf_add_elapsed()` now return immediately when `start_ns == 0`.
When `start_ns` is non-zero, they still recheck the corresponding enabled flag
before recording elapsed time, preserving the previous behavior where disabling
stats before scope exit suppresses the final update.

The enabled path writes directly to the counter array after the recheck instead
of going through the add helper, avoiding a redundant second enabled-flag load
when stats are enabled.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli adapter, wire-protocol, storage-engine,
directory-layout, checkpoint, page-version WAL, or recovery behavior changes.
This is an internal diagnostics fast path.

## Directory And Lifecycle Impact

No durable files, shared-memory layouts, checkpoint files, or runtime directory
lifecycle rules change. Embedded open/close timing counters keep the same
external test hook behavior.

## Native Storage Impact

No native InnoDB, MyISAM, Aria, redo, undo, page, tablespace, or dictionary
format changes. No MariaDB-derived source files are changed.

## Build, Size, License, And Dependencies

No dependency or license impact. Binary-size impact is limited to two small
helper branches in first-party C++ code.

## Test And Verification Plan

- Build production `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe`.
- Run `single-owner-native-support-page-wal-elision`, which now:
  - verifies enabled database perf counters still report native-support
    page-index skip work,
  - disables and resets database perf counters,
  - runs an ownerless read,
  - verifies representative disabled database counters remain zero.
- Run adjacent ownerless selectors for history proof, explicit transaction
  latest-checkpoint coalescing, and multi-row visible-fast behavior.
- Run a reduced stats-off production probe and a stats-enabled attribution probe
  to ensure summaries still emit.
- Run hook crash selectors around page-visible/checkpoint publication, focused
  ownerless stress selectors, production-build audit, formatting, and
  whitespace checks.

## Implementation Evidence

Local verification used production embedded binaries for timing-sensitive
commands:

- `cmake --build build/php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe
  -j$(nproc)` passed.
- Production selectors passed:
  `single-owner-native-support-page-wal-elision`,
  `single-owner-history-wal-proof`,
  `explicit-transaction-undo-wal-elision`, and
  `single-owner-multi-row-insert-visible-fast-path`.
- Reduced stats-off probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=1`,
  `MYLITE_PERF_INSERT_ITERATIONS=400`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`.
  The sample reported ordinary explicit transaction inserts at
  `4135.10 ops/s`, ownerless explicit transaction inserts at
  `946.45 ops/s`, ratio `0.2289`; ordinary autocommit inserts at
  `1270.26 ops/s`, ownerless autocommit inserts at `925.71 ops/s`, ratio
  `0.7288`; ordinary autocommit bulk rows at `12455.45 rows/s`, ownerless
  autocommit bulk rows at `2746.85 rows/s`, ratio `0.2205`; ordinary active
  runtime reconnect at `0.928 ms`, ownerless active runtime reconnect at
  `1.376 ms`.
- Reduced stats-enabled probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=1`,
  `MYLITE_PERF_INSERT_ITERATIONS=60`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, and
  `MYLITE_PERF_OWNERLESS_PAGE_LOG_DETAIL_STATS=1`.
  The probe emitted checkpoint, redo, page-publish, page-log, page-write,
  row/undo, and deferred latest-checkpoint summaries; for example explicit
  transactions still coalesced one deferred latest checkpoint per insert, and
  autocommit still coalesced two deferred latest checkpoints per insert.
- `cmake --build build/ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j$(nproc)` passed.
- Hook selectors passed: `visible-publish-crash`,
  `visible-checkpoint-crash`, `redo-written-crash`, `redo-latest-crash`, and
  `redo-latest-checkpoint-crash`.
- `cmake --build build/ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j$(nproc)` passed.
- Ownerless stress subset passed under `build/ownerless-stress`:
  `libmylite.ownerless-single-owner-native-support-page-wal-elision`,
  `libmylite.ownerless-single-owner-multi-row-insert-visible-fast-path`, and
  `libmylite.ownerless-cross-process-checksum-stress`.
- `tools/check-ci-production-builds`,
  `cmake --build --preset format-check-prod`, and `git diff --check` passed.

## Acceptance Criteria

- Stats-disabled database and embedded-open elapsed helpers return before
  rechecking the disabled stats flag.
- Stats-enabled helper behavior and counter values remain intact.
- Focused ownerless SQL coverage proves disabled database counters stay zero.
- Production, hook, stress, formatting, and production-build guard checks pass.

## Risks And Follow-Up

- This is a micro-optimization. It should reduce diagnostic overhead in normal
  stats-off paths, but it is not expected to close the main ownerless write
  performance gap by itself.
- Remaining performance work still needs larger proof around native
  row/undo mini-transaction cost, history-proof publication volume,
  page-version append volume, and redo/checkpoint reconciliation.
