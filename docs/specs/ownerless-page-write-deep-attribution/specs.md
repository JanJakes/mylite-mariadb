# Ownerless Page Write Deep Attribution

## Goal

Expose production-shaped InnoDB deep performance counters for ownerless
page-write MTR work so the remaining non-empty-table bulk insert gap can be
assigned to lock acquire, transaction-deferred page tracking, native-support
hits, or page-image capture without enabling page-publish/page-write diagnostic
modes that alter some fast paths.

## Non-Goals

- Do not broaden MariaDB's empty-table `TRX_UNDO_EMPTY` bulk insert path to
  non-empty tables.
- Do not skip undo record creation, rollback-to-statement-start behavior,
  savepoint behavior, history proof publication, or native page-version WAL.
- Do not change ownerless lock bytes, WAL format, checkpoint ordering, shared
  memory layout, SQL behavior, or public MyLite APIs.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0rec.cc::trx_undo_report_row_operation()`
  already exposes aggregate `trx_undo_report_mtr_commit` time, but that bucket
  does not distinguish ownerless page-write enter/acquire work from native MTR
  commit work.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_enter()`
  decides whether a page is transaction-deferred, native-support-held, already
  tracked by the MTR, already owned by the transaction, or requires an ownerless
  page-write lock acquisition.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_capture_dirty_transaction_page()`
  captures the page images that commit-time visible-fast publication later
  appends to the ownerless page log.
- Existing `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` and
  `MYLITE_PERF_OWNERLESS_PAGE_WRITE_STATS=1` probes are useful diagnostics but
  enable broader counters that intentionally disable some stats-off
  native-support fast skips.

## Compatibility Impact

None for SQL, C API, mysqli/PHP behavior, storage format, metadata, wire
protocol, or directory layout. The new counters are opt-in diagnostics behind
the existing InnoDB deep performance machinery. Normal production runs leave
the counters disabled.

`docs/COMPATIBILITY.md` should mention the new lightweight
`MYLITE_PERF_OWNERLESS_INNODB_DEEP_STATS=1` probe mode because it changes the
available performance evidence, not supported behavior.

## Design

Add deep InnoDB counters for the ownerless page-write MTR path:

- `ownerless_page_write_enter_*` counts and times enter-path work;
- gate and physical page acquire timers separate transaction gate acquisition
  from page-write lock acquisition;
- transaction-owned, transaction-dirty, MTR-duplicate, and native-support-hit
  counters expose early returns;
- transaction page note, dirty page note, and page-image capture counters show
  how much work survives to commit-time visible-fast publication.

Add `MYLITE_PERF_OWNERLESS_INNODB_DEEP_STATS=1` to the embedded performance
probe. This mode enables only InnoDB deep counters and first/remaining bulk
deep snapshots. It does not enable page-publish stats, page-write perf stats,
page-log append stats, database perf stats, or exec-result profiling.

Existing page-publish attribution remains unchanged and still enables the
broader diagnostics needed for publication-volume analysis.

## File Lifecycle

No durable files, temporary files, cleanup rules, recovery files, lock files,
or MyLite database-directory contents change. The counters live in process
memory and are reset by the existing probe reset path.

## Embedded Lifecycle And API

No `libmylite` public API changes. The embedded performance probe gains one
environment variable and extra output rows when the variable is enabled.

## Build, Size, And Dependencies

No dependency or license changes. The slice touches upstream-derived InnoDB MTR
and deep perf headers plus the first-party performance probe, so the embedded
MariaDB archive must be rebuilt before running production embedded probes.

## Test Plan

- Rebuild the MariaDB embedded archive.
- Build `mylite_embedded_performance_probe` and the focused ownerless SQL test
  target with `php-embedded-prod`.
- Run a reduced 100-row-per-statement production probe with
  `MYLITE_PERF_OWNERLESS_INNODB_DEEP_STATS=1` and confirm:
  - the new flag prints as enabled;
  - raw ownerless page-write deep rows are emitted;
  - first and remaining bulk deep comparison rows include page-write enter,
    acquire, native-support hit, transaction-owned/dirty skip, and capture
    metrics.
- Run a matching stats-off reduced production probe to confirm the normal
  throughput path still emits existing ratios.
- Run focused ownerless visible-fast/native-support/history selectors.
- Run production build guards, format, and `git diff --check`.

## Acceptance Criteria

- Normal stats-off production probes keep the existing hot path and output.
- Lightweight deep mode emits ownerless page-write MTR attribution without
  enabling page-publish or page-write diagnostic modes.
- First/remaining bulk summaries make the non-empty-table path visible.
- Compatibility docs record the new evidence boundary and avoid claiming a
  throughput fix.

## Risks And Open Questions

- Deep counters still add timer and atomic overhead while enabled, so they are
  attribution evidence, not throughput evidence.
- The slice does not by itself reduce the ownerless write gap. It should make
  the next optimization target concrete.
- Broader native undo optimization remains correctness-sensitive because
  rollback, savepoint, history proof, peer visibility, crash/reopen, and
  native recovery semantics depend on the existing undo and page-version paths.

## Verification Results

- `tools/mariadb-embedded-build build` rebuilt the embedded MariaDB archive.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  rebuilt the production probe and focused ownerless SQL target.
- Reduced production deep-attribution probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=1 MYLITE_PERF_INSERT_ITERATIONS=500
  MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
  MYLITE_PERF_OWNERLESS_INNODB_DEEP_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`.
  The output showed `mylite_perf_ownerless_innodb_deep_stats=1`,
  `mylite_perf_ownerless_page_publish_stats=0`, and
  `mylite_perf_ownerless_page_write_stats=0`, confirming the lightweight mode
  avoided the broader diagnostic modes. It emitted ownerless page-write enter,
  page-acquire, native-support, transaction-owned skip, and capture-image rows.
- The same probe attributed the remaining non-empty bulk path to repeated
  ownerless page-write enter checks and dirty image capture work:
  remaining statements had about `306.500` page-write enter calls per
  statement, `4.000` physical page-acquire calls per statement, and `99.000`
  capture-image updates per statement. The measured enter, gate-acquire,
  page-acquire, and capture-image timers stayed below the larger
  `trx_undo_report_mtr_commit` and row-insert buckets, which keeps the next
  optimization target in the native undo/MTR write path instead of lock
  acquisition alone.
- Matching stats-off reduced production probe kept the normal output path and
  showed the remaining performance gap: ownerless 500-row autocommit bulk ran
  at `0.1146x` ordinary, with remaining statements at `0.0950x`.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|native-support-page-wal-elision|history-wal-proof)|uncommitted-peer-hidden)$'
  --output-on-failure` passed.
- Direct production checks passed:
  `mylite_ownerless_cross_process_sql_test active-reader-pressure` and
  `mylite_ownerless_cross_process_sql_test commit-race`.
- `tools/check-ci-production-builds`,
  `cmake --build --preset format-check-prod`, and `git diff --check` passed.
