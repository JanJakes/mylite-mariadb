# Ownerless Page Image Last-Hit Cache

## Goal

Reduce ownerless non-empty bulk-insert overhead in the InnoDB page-image
capture path by avoiding repeated linear scans of the transaction-deferred page
image vector when successive mini-transactions update the same packed page.

## Non-Goals

- Do not skip native undo records, mini-transaction commits, rollback,
  savepoint, or history-list behavior.
- Do not change ownerless page-write acquisition, page-visible LSN publication,
  page-log format, checkpoint ordering, recovery, or lock-file layout.
- Do not broaden SQL fast-path eligibility, row-list limits, DDL coverage, or
  cross-process concurrency claims.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `trx_undo_report_row_operation()` still owns native row/undo MTR work for
  non-empty inserts. That broader undo path remains correctness-sensitive.
- `mtr_t::ownerless_page_write_capture_dirty_transaction_page()` captures the
  latest transaction-deferred page image while the page is latched.
- The captured images live in `trx_t::mylite_ownerless_page_images` until
  commit-time page publication consumes them.
- The existing capture path used `std::find_if()` over that vector for every
  captured page, even when a bulk insert repeatedly updated the same page.
- `trx_t` already keeps last-hit caches for ownerless modified-page,
  dirty-page, and native-support membership checks, with clear/reset paths tied
  to transaction lifecycle cleanup.

## Compatibility Impact

None for SQL behavior, public C API, PHP/mysqli behavior, durable files, WAL
format, checkpoint format, lock bytes, or directory lifecycle. The authoritative
page-image vector stays unchanged; the new cache is process-local transaction
bookkeeping and is validated before use.

`docs/COMPATIBILITY.md` records this as a bounded ownerless performance
optimization, not a completion claim for native row/undo performance.

## Design

Add a transaction-local last-hit index for the page-image vector:

- cache state is initialized and invalidated with the existing ownerless
  transaction page state;
- capture validates `index < images.size()` and
  `images[index].packed_page == packed_page` before using the cached entry;
- any invalid, stale, or different-page cache state falls back to the existing
  `std::find_if()` scan;
- fallback scan refreshes the cache after finding an existing image;
- insert refreshes the cache to the newly appended image index.

Add deep performance counters for cache hits and misses so the production
performance probe can show whether bulk inserts are actually bypassing scans.

## File Lifecycle

No durable files or temporary files change. No new cleanup responsibility is
added outside the existing transaction cleanup paths that already clear
ownerless modified pages, dirty pages, native-support pages, and captured page
images.

## Embedded Lifecycle And API

No public `libmylite` API changes. The embedded performance probe gains two
additional InnoDB deep output rows when deep stats are enabled:

- `ownerless_page_write_capture_image_cache_hits`
- `ownerless_page_write_capture_image_cache_misses`

## Build, Size, And Dependencies

No dependency or license changes. The slice touches upstream-derived InnoDB
headers and MTR code, so the embedded MariaDB archive must be rebuilt before
production embedded verification.

## Test Plan

- Rebuild the MariaDB embedded archive.
- Build `mylite_embedded_performance_probe` and the focused ownerless SQL test
  target with `php-embedded-prod`.
- Run a reduced production deep probe with
  `MYLITE_PERF_OWNERLESS_INNODB_DEEP_STATS=1` and confirm cache-hit/cache-miss
  rows are emitted and hits are positive for repeated bulk inserts.
- Run focused ownerless visible-fast, native-support, history-proof, and
  uncommitted-peer selectors.
- Run active-reader pressure and commit-race direct checks.
- Run production build guards, format, and `git diff --check`.

## Acceptance Criteria

- Page-version, native-support, history-proof, and visible-fast publication
  tests keep passing.
- Cache hit/miss counters are visible in the production deep probe.
- Repeated same-page image updates record cache hits.
- No durable format, SQL, public API, or recovery claim changes.

## Risks And Open Questions

- The cache reduces lookup overhead only when repeated captures target the same
  page; it does not remove the larger native undo/MTR cost.
- Broad native undo reductions remain risky until rollback, savepoint, MVCC,
  history proof, peer visibility, crash/reopen, and recovery behavior are
  designed and tested together.

## Verification Results

- `tools/mariadb-embedded-build build` rebuilt the embedded MariaDB archive.
- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe mylite_ownerless_cross_process_sql_test`
  rebuilt the production performance probe and focused ownerless SQL target.
- Reduced production deep probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=1 MYLITE_PERF_INSERT_ITERATIONS=500
  MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=100
  MYLITE_PERF_OWNERLESS_INNODB_DEEP_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
  passed and emitted the new cache rows. Remaining ownerless bulk statements
  reported `98.500` capture-image cache hits per statement and `2.500`
  cache misses per statement, while preserving `99.000` capture-image updates
  per statement and `2.000` capture-image inserts per statement.
- Focused CTest selector passed:
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(single-owner-(multi-row-insert-visible-fast-path|native-support-page-wal-elision|history-wal-proof)|uncommitted-peer-hidden)$'
  --output-on-failure`.
- Direct focused checks passed:
  `mylite_ownerless_cross_process_sql_test active-reader-pressure` and
  `mylite_ownerless_cross_process_sql_test commit-race`.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test` rebuilt the stress target in the
  `Release` ownerless-stress build tree.
- `ctest --preset ownerless-stress --output-on-failure` passed all 12
  ownerless cross-process stress cases in `475.09` seconds.
- `tools/check-ci-production-builds`,
  `tools/require-cmake-release-build build/php-embedded-prod
  build/ownerless-stress`, `cmake --build --preset format-check-prod`, and
  `git diff --check` passed.
