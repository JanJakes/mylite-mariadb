# Ownerless Page Publish Stats-Off Fast Path

## Problem

Normal production, CI timing, WordPress PHPUnit, and default embedded
performance runs keep ownerless page-publish attribution counters disabled.
The native page publication hot path still calls page-publish attribution
helpers for every candidate, published, elided, and failed page. Most helpers
return after reloading the disabled stats flag, but the caller has already paid
the function call and in some cases page-class or history-proof attribution
setup.

This is not the main ownerless write-throughput bottleneck. Current production
attribution still points at native mini-transaction commit work, two history
proof page-version publications for autocommit inserts, page-log append, and
broader redo/checkpoint reconciliation. This slice removes avoidable
diagnostic overhead from the stats-disabled page-publish path without changing
publication, WAL, checkpoint, or recovery semantics.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::ownerless_page_write_publish()` owns ownerless page-version
  publication from committed mini-transactions.
- The same file owns page-publish attribution through
  `ownerless_page_publish_stats_enabled`,
  `ownerless_page_publish_count*()`, history-proof role counters, TRX_SYS diff
  sampling, and history-proof diff sampling.
- Page publication itself does not depend on page-publish stats. Required
  correctness side effects are separate:
  `ownerless_page_write_note_publish_success()`,
  `ownerless_page_write_note_publish_failure()`, and
  `ownerless_page_write_note_history_proof_page()`.
- A fresh production probe on this branch reported default stats-off ownerless
  explicit and autocommit insert ratios near `0.50x` ordinary and four-row
  bulk insert ratio near `0.34x`; stats-enabled attribution reported about
  `3.000` page versions per autocommit insert, `2.000` published
  native-support history-proof pages per insert, page-log append around
  `0.087 ms/insert`, page-write publish around `0.132 ms/insert`, and native
  write-history/undo/row MTR commit work as separate nonzero costs.

## Scope And Non-Goals

In scope:

- Hoist the stats-disabled decision once inside
  `mtr_t::ownerless_page_write_publish()`.
- Skip page-publish attribution helper calls when page-publish stats are
  disabled.
- Keep the enabled stats path and existing counter names/order intact.
- Extend focused ownerless SQL coverage so a write executed while page-publish
  stats are disabled leaves representative page-publish counters at zero.
- Update compatibility/performance docs with the measured boundary.

Out of scope:

- Reducing page-version record count.
- Replacing history-proof page publication.
- Changing page-log payload format, checkpoint rewrite, recovery, native
  InnoDB redo, undo, row, or mini-transaction semantics.
- SQL-level table-lock fault injection, broader DDL/file lifecycle recovery,
  active-reader pressure policy, or external MariaDB/RQG stress.

## Design

`mtr_t::ownerless_page_write_publish()` reads
`ownerless_page_publish_stats_enabled` once after the existing publication
eligibility checks. Page-publish counter and attribution helper calls are
guarded by that boolean. When disabled, publication continues through the same
source-page validation, native-support elision, page copy/checksum, page-log
append hook, publish-success/failure marking, and history-proof success marking
without invoking diagnostics-only helpers.

`ownerless_page_write_can_elide_native_support_page()` receives the same
stats-enabled boolean so its history-proof-elision-blocked counters are skipped
when diagnostics are disabled while preserving the native-support elision
decision.

When stats are enabled, the same helpers run and keep their existing internal
enabled checks. This keeps the diagnostic path conservative and avoids changing
counter behavior outside the disabled fast path.

## Compatibility Impact

No SQL, public C API, PHP API, mysqli adapter, wire-protocol, directory-layout,
storage-format, native page, redo, undo, or checkpoint behavior changes. This
is an internal diagnostics fast path.

## Directory And Lifecycle Impact

No durable files, shared-memory layouts, page-version WAL records, checkpoint
files, runtime directory lifecycle rules, or cleanup paths change.

## Native Storage Impact

No native InnoDB page format or recovery behavior changes. Page publication
still copies and prepares the committed page image before appending it to the
ownerless page-version WAL, and history-proof success is still recorded only
after publication succeeds.

## Build, Size, License, And Dependencies

No dependency or license impact. Because the changed source is MariaDB-derived
InnoDB code, verification must rebuild the embedded MariaDB archive before
rebuilding first-party embedded targets. Binary-size impact is limited to
small branches around diagnostics-only calls.

## Test And Verification Plan

- Rebuild the production MariaDB embedded archive.
- Rebuild production `mylite_ownerless_cross_process_sql_test`,
  `mylite_ownerless_primitives_test`, and
  `mylite_embedded_performance_probe`.
- Run `single-owner-native-support-page-wal-elision`, which now verifies both
  enabled page-publish counters and a disabled write that leaves
  representative page-publish counters at zero.
- Run adjacent production ownerless selectors for history proof, visible-fast
  multi-row inserts, and explicit transaction undo/history proof.
- Run a reduced stats-off production probe and a reduced stats-enabled
  attribution probe to confirm summaries still emit.
- Run hook crash/fallback selectors around page-visible/checkpoint and
  history-proof publication.
- Run focused ownerless stress selectors, production-build guards, formatting,
  and whitespace checks.

## Implementation Evidence

Local verification used `build/mariadb-embedded` with the production
`MinSizeRel` baseline and `build/php-embedded-prod` with first-party `Release`
artifacts:

- `tools/mariadb-embedded-build build` rebuilt
  `build/mariadb-embedded/libmysqld/libmariadbd.a`.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test
  mylite_embedded_performance_probe -j$(nproc)` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-native-support-page-wal-elision` passed. The selector now
  verifies enabled native-support and history-proof page-publish counters,
  performs a stats-disabled ownerless insert, and checks representative
  disabled page-publish counters remain zero.
- Adjacent production selectors passed:
  `single-owner-history-wal-proof`,
  `explicit-transaction-undo-wal-elision`, and
  `single-owner-multi-row-insert-visible-fast-path`.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_primitives_test`
  passed.
- Reduced stats-off production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=2`,
  `MYLITE_PERF_SELECT_ITERATIONS=5`,
  `MYLITE_PERF_INSERT_ITERATIONS=1000`, and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`.
  The sample reported ordinary explicit transaction inserts at
  `3552.94 ops/s`, ownerless explicit transaction inserts at
  `2455.12 ops/s`, ratio `0.6910`; ordinary autocommit inserts at
  `2995.38 ops/s`, ownerless autocommit inserts at `1708.75 ops/s`, ratio
  `0.5705`; ordinary autocommit bulk rows at `8835.73 rows/s`, ownerless
  autocommit bulk rows at `3071.46 rows/s`, ratio `0.3476`; ordinary active
  runtime reconnect at `0.801 ms`, ownerless active runtime reconnect at
  `1.722 ms`.
- Reduced stats-enabled production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=1`,
  `MYLITE_PERF_INSERT_ITERATIONS=200`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.
  The probe still reported `2.000` native-support pages published per
  autocommit insert, exactly `1.000` history-proof rollback-segment page and
  `1.000` history-proof undo page per insert, page-log append at
  `0.076 ms/insert`, page-write publish at `0.112 ms/insert`,
  write-history at `0.104 ms/insert`, `trx_commit_for_mysql` at
  `0.138 ms/insert`, clustered row-insert MTR commit at `0.053 ms/insert`,
  and undo-report MTR commit at `0.028 ms/insert`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j$(nproc)` passed.
- Hook selectors passed: `history-proof-publish-failure-fallback`,
  `visible-publish-crash`, `visible-checkpoint-crash`, `redo-written-crash`,
  `redo-latest-crash`, and `redo-latest-checkpoint-crash`.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j$(nproc)` passed.
- Ownerless stress subset passed under `build/ownerless-stress`:
  `libmylite.ownerless-single-owner-history-wal-proof`,
  `libmylite.ownerless-single-owner-native-support-page-wal-elision`,
  `libmylite.ownerless-single-owner-multi-row-insert-visible-fast-path`, and
  `libmylite.ownerless-cross-process-checksum-stress`.
- `tools/check-ci-production-builds`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `tools/require-cmake-release-build build/ownerless-test-hooks`,
  `tools/require-cmake-release-build build/ownerless-stress`,
  `cmake --build --preset format-check-prod`, and `git diff --check` passed.

## Acceptance Criteria

- Stats-disabled page-publish helper work is skipped from the publish hot path.
- Stats-enabled page-publish counters and attribution remain intact.
- Focused SQL coverage proves disabled page-publish counters stay zero through
  an ownerless write.
- Production, hook, stress, formatting, and production-build guard checks pass.

## Risks And Follow-Up

- The disabled fast path snapshots the stats-enabled flag once per publish
  call. Page-publish stats are diagnostic test/probe controls, not a
  synchronization primitive; enabling stats concurrently with an in-flight
  publication may miss that publication just as any lock-free diagnostic
  sampling can.
- This is a bounded hot-path cleanup. The remaining performance targets are
  still native commit/page-publication cost, history-proof proof volume,
  page-log append/encoding, and broader redo/checkpoint reconciliation.
