# Ownerless Native Support Publish Fast Skip

## Problem

The stats-enabled 2048-row ownerless bulk insert probe shows that later
statements still spend measurable time in the undo-report mini-transaction
commit path. The previous attribution slice separated the work and showed that
the hot path is not fresh undo-log creation: cached undo is reused and the
remaining cost is dominated by the no-dirty mini-transaction commit loop,
including native-support page publication checks and page-write release.

For normal production runs the page-publish and page-write diagnostic counters
are disabled. In that mode, calling the full ownerless page-publish helper for a
page that the existing native-support rules will only elide is a no-op: it does
not publish a page-version record, does not mark publish success, and does not
change transaction visibility. It still repeats the helper dispatch and
classification on every eligible undo/native page.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0rec.cc`
  `trx_undo_report_row_operation()` writes a native undo record for each
  non-empty bulk insert row and commits a mini-transaction for that undo record.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::commit_log()` publishes modified pages in both made-dirty and
  no-dirty commit branches. In the no-dirty branch this includes undo pages
  whose page-version publication is already elided by
  `ownerless_page_write_can_elide_native_support_page()`.
- The same helper refuses active rollback-segment/undo history-proof pages and
  is already the authority for native-support publication elision.
- `docs/specs/ownerless-explicit-txn-undo-elision/specs.md` records the
  existing invariant that native undo records and redo remain native, while
  eligible pre-commit undo page-version WAL can be skipped when history-proof
  pages are not involved.

## Design

Add a production fast skip before ownerless page publication:

- run only when page-publish stats and page-write perf stats are both disabled;
- require a persistent in-file page with a readable source image;
- require the page image LSN to equal the mini-transaction commit LSN;
- reuse `ownerless_page_write_can_elide_native_support_page()` with stats
  counting disabled;
- leave transaction-deferred user page publishing unchanged;
- leave ownerless page-write acquisition, release, redo, native undo records,
  history-proof publication, and dirty-page commit bookkeeping unchanged.

The fast skip is used in the made-dirty publish-list path, the made-dirty
publish-scan fallback, and the no-dirty commit loop. Diagnostic runs keep the
old helper path so existing page-publish and page-write counters continue to
show the detailed publication attempts.

## Scope And Non-Goals

In scope:

- production stats-off ownerless native-support publish elision;
- `mtr0mtr.cc` page-publish dispatch;
- focused ownerless correctness verification and reduced production perf
  comparison.

Out of scope:

- skipping native undo records or row rollback records;
- holding undo page-write locks until commit;
- changing the active history-proof rollback-segment/undo pages;
- changing page-version WAL format, redo ordering, checkpoint policy, or SQL
  visibility rules.

## Compatibility Impact

No SQL, C API, PHP API, storage-format, or directory-layout behavior changes.
The optimization only removes stats-off calls that would take the existing
native-support elision no-op path.

## Native Storage Impact

MariaDB still writes native undo pages, redo records, and transaction history.
Pages that require ownerless page-version publication or active history proof
continue through the existing publish helper.

## Test And Verification Plan

- Build the production embedded targets touched by the slice.
- Run focused ownerless native-support/history-proof selectors, including
  explicit transaction undo elision and commit-race coverage.
- Run the ownerless primitive subset.
- Run a reduced stats-off production performance probe and compare ownerless
  bulk insert page-write/undo timings with the prior reduced samples.
- Run production build guards, format check, and whitespace checks.

## Acceptance Criteria

- Existing native-support/history-proof SQL selectors pass.
- Stats-enabled selectors continue to observe native-support elision and
  history-proof publication counters because the fast skip is disabled when
  diagnostics are enabled.
- Stats-off reduced production probe shows no correctness failure and records
  ownerless/ordinary timing for comparison with prior local samples.
- Docs state that this is a dispatch fast path, not a broader concurrency or
  undo-record elision.

## Verification Results

Completed on 2026-06-20 with `build/mariadb-embedded` at `MinSizeRel` and
`build/php-embedded-prod` at `Release`:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R '^libmylite\.ownerless-primitives$'
  --output-on-failure`
- `mylite_ownerless_cross_process_sql_test
  explicit-transaction-undo-wal-elision`
- `mylite_ownerless_cross_process_sql_test
  single-owner-native-support-page-wal-elision`
- `mylite_ownerless_cross_process_sql_test single-owner-history-wal-proof`
- `mylite_ownerless_cross_process_sql_test commit-race`
- `mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.(embedded-ownerless-(mdl-hooks|trx-hooks|innodb-lock-hooks)|ownerless-single-owner-(page-write-refresh-skip|external-refresh-skip))$'
  --output-on-failure`
- stats-off reduced production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1 MYLITE_PERF_SELECT_ITERATIONS=20
  MYLITE_PERF_INSERT_ITERATIONS=20480
  MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=2048
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=0
  MYLITE_PERF_OWNERLESS_APPEND_STATS=0
  MYLITE_PERF_OWNERLESS_PAGE_LOG_DETAIL_STATS=0
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- `tools/check-ci-production-builds`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The saved stats-off probe at
`build/manual-ownerless-native-support-publish-fast-skip-statsoff-2048.log`
completed without correctness failures and reported ownerless bulk rows at
`39647.75` rows/s versus ordinary `148163.07` rows/s, ratio `0.2676`;
remaining ownerless bulk rows reported `36698.37` rows/s versus ordinary
`149407.75` rows/s, ratio `0.2456`. An immediately preceding unsaved run of
the same command reported a higher ownerless bulk ratio (`0.3573`) and
remaining ratio (`0.3283`), so this slice records the fast-path guard and
correctness evidence but does not claim that the broader bulk performance gap
is closed.
