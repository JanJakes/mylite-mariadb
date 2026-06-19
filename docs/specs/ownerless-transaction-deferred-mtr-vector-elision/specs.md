# Ownerless Transaction-Deferred MTR Vector Elision

## Problem

The ownerless bulk-insert profile still shows the write path, not process
startup, as the largest performance gap. After earlier page-log append,
release-membership, and release-exhaustion slices, a reduced production probe
at `03a414e0` still reported a 100-row bulk statement spending time in the
ownerless no-dirty MTR commit-log loop even though only a small number of page
versions were ultimately published:

- `mylite_perf_summary_ownerless_autocommit_bulk_page_versions_per_statement=2.000`;
- `mylite_perf_summary_ownerless_autocommit_bulk_page_write_commit_log_no_dirty_loop_ms_per_statement=0.766`;
- `mylite_perf_summary_ownerless_autocommit_bulk_page_write_commit_log_no_dirty_page_leave_ms_per_statement=0.477`.

The no-dirty loop must still release native latches, but data pages that are
classified for transaction-deferred ownerless publication are already owned by
the transaction page-write registry. Recording those same pages in the
mini-transaction page-write vector makes the MTR release loop rediscover that
the release is deferred.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_enter()`
  computes `holds_for_transaction` after lock-only, read-only, startup, and
  recovery checks.
- When `holds_for_transaction` is true and the page-write hook acquires a
  page, the page is added to the transaction modified-page registry by
  `ownerless_page_write_note_transaction_page()`.
- `mtr_t::ownerless_page_write_leave()` removes pages from
  `m_ownerless_page_write_mtr_pages` only to decide whether the ownerless
  page-write lock should be released by this mini-transaction or deferred to
  transaction cleanup.
- Existing release-loop fast paths already prove that non-member pages do not
  need the MTR leave helper.

## Design

Keep the transaction-deferred page in exactly one ownerless ownership registry:

- if `ownerless_page_write_enter()` acquires a page-write lock for a
  transaction-deferred page, record it in the transaction page-write registry
  and increment a diagnostic counter;
- do not also add that page to `m_ownerless_page_write_mtr_pages`;
- before acquiring a page-write lock, treat an already transaction-owned page
  as a duplicate acquisition just as the old MTR-vector membership check did
  for MTR-owned pages;
- keep MTR-vector tracking unchanged for pages whose ownerless page-write lock
  must be released by the mini-transaction.

The change preserves the existing transaction release hook, commit-time
transaction page publication, page-version WAL records, commit visibility, and
native latch release order. It only avoids inserting transaction-owned pages
into a vector that the MTR release loop would later erase without releasing
the ownerless page-write lock.

## Compatibility Impact

No SQL behavior, public C API, PHP API, mysqli behavior, wire protocol,
storage-engine file format, page-version WAL format, checkpoint format, or
directory lifecycle behavior changes. This is an internal ownerless InnoDB
write-path optimization.

## Directory And Lifecycle Impact

No durable files, shared-memory record formats, page-log records, checkpoint
records, or cleanup paths are added or changed.

## Native Storage Impact

Native InnoDB page latches, redo, undo, flush-list handling, page-version
publication, and transaction commit cleanup remain on the existing paths.
Mini-transaction-owned pages still use the existing MTR leave helper before
native latch unlock. Transaction-owned pages still release through the native
bulk page-write release path during transaction cleanup.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mtr0mtr.cc`.
- Build production embedded ownerless SQL and performance-probe targets.
- Extend `ownerless-single-owner-multi-row-insert-visible-fast-path` to assert
  transaction-deferred page publication is still active and the new
  transaction-deferred MTR-vector elision counter is positive.
- Run focused production selectors covering multi-row visible-fast insert,
  history WAL proof, native-support page WAL elision, FK fast-path cache, and
  uncommitted peer visibility.
- Run hook/stress selectors covering ownerless page-write release, crash, and
  active-reader pressure.
- Run reduced stats-enabled and stats-off production probes to compare the
  no-dirty page-leave bucket and preserve page-version/native-support/commit
  visibility counts.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Transaction-deferred page-write acquisitions are tracked in the transaction
  page registry without also populating the MTR page-write vector.
- MTR-scoped page-write acquisitions still use the MTR vector and release
  helper.
- Focused ownerless correctness selectors pass.
- Reduced production probe counts preserve visible-fast commit publication,
  transaction-deferred page publication, native-support history proof, and zero
  publish failures.

## Verification Results

Local verification used `build/mariadb-embedded` with the documented
`MinSizeRel` baseline and `build/php-embedded-prod` production targets.

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-single-owner-multi-row-insert-visible-fast-path$'
  --output-on-failure`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(primitives|single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --parallel 2 --output-on-failure`
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-(primitives|single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|negative-proof|history-proof-publish-failure-fallback)$'
  --parallel 2 --output-on-failure`
- `cmake --build --preset ownerless-stress`
- `ctest --preset ownerless-stress --output-on-failure`
- `tools/check-ci-production-builds`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `tools/require-cmake-release-build build/ownerless-test-hooks`
- `tools/require-cmake-release-build build/ownerless-stress`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The focused SQL selector asserts commit visibility stays on the fast path,
conservative flush and publish-failure counters stay zero, transaction-deferred
page image/buffer publication remains active, page-log append batching remains
active, and the new
`OWNERLESS_TEST_PAGE_WRITE_PERF_STAT_TRANSACTION_DEFERRED_MTR_ELIDED` counter
is positive.

A reduced stats-enabled production probe with
`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
`MYLITE_PERF_SELECT_ITERATIONS=20`,
`MYLITE_PERF_INSERT_ITERATIONS=100`,
`MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100`,
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, and
`MYLITE_PERF_OWNERLESS_PAGE_LOG_DETAIL_STATS=1` preserved the expected 100-row
bulk statement shape:

- transaction-deferred MTR-vector elisions: `2`;
- page versions per statement: `2.000`;
- native-support published pages per statement: `2.000`;
- native-support elided pages per statement: `100.000`;
- commit-visibility fast per statement: `1.000`;
- commit-visibility flush per statement: `0.000`;
- no-dirty page-leave time per statement: `0.187 ms`;
- no-dirty loop time per statement: `0.362 ms`;
- page-write commit-log time per statement: `1.462 ms`.

For comparison, the pre-slice same-shape reduced sample at `03a414e0` reported
no-dirty page-leave time `0.477 ms/statement`, no-dirty loop time
`0.766 ms/statement`, and page-write commit-log time `2.253 ms/statement`.

A reduced stats-off production smoke with
`MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
`MYLITE_PERF_SELECT_ITERATIONS=100`,
`MYLITE_PERF_INSERT_ITERATIONS=500`, and
`MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100` reported ownerless 100-row
bulk inserts at `25398.94 rows/s` versus ordinary `111217.58 rows/s`, ratio
`0.2284`. Treat this as a local smoke result, not a benchmark guarantee.

## Risks And Follow-Up

This is a bounded write-path cleanup. It does not remove the remaining
history-proof/native-support proof records, change redo/checkpoint
reconciliation, or broaden SQL-level concurrency coverage. Those remain larger
correctness and performance slices.
