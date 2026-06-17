# Ownerless Dirty Capture Classification Reuse

## Problem

Ownerless explicit transactions defer many modified page images to
transaction-level publication. Several MTR paths already compute that a page is
held for transaction publication before calling
`ownerless_page_write_note_dirty_transaction_page()` and
`ownerless_page_write_capture_dirty_transaction_page()`. The dirty-page note
helper can accept that precomputed classification, but the capture helper still
rechecks ownerless hook state and recomputes
`ownerless_page_write_publishes_with_transaction()` before it captures the page
image.

Current production attribution keeps explicit transaction performance below
ordinary MariaDB and points remaining cost at ownerless page publication and
MTR commit work. The capture path should reuse existing per-page
classification where callers already proved it, without weakening the generic
defensive entry point.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_writes_publish_list()`
  computes `transaction_publish` before choosing transaction-deferred capture
  versus immediate page publication.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_writes_publish()`
  computes the same `transaction_publish` classification while scanning the
  MTR memo.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::commit_log()` computes the
  same predicate in its no-dirty modified-page release loop.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_write_capture_dirty_transaction_page()`
  currently checks hook state and recomputes
  `ownerless_page_write_publishes_with_transaction()` before reading the page
  image, even for callers that are already inside a proven transaction-publish
  branch.
- The capture helper still validates transaction availability, source page
  availability, page ownership, and page LSN before recording an image.

## Design

Add a classified overload of
`ownerless_page_write_capture_dirty_transaction_page()` that accepts
`transaction_release_holds_page`. The existing no-argument capture helper
remains the generic defensive entry point: it keeps the hook check and computes
the predicate before delegating to the classified overload.

Update the three MTR publish paths that already know the transaction-publish
result to call the classified overload. The classified overload still returns
immediately when the precomputed value is false and still validates the
transaction pointer, source page, transaction-owned page set, and page LSN
before storing or replacing an image.

## Compatibility Impact

No SQL semantics, public C API, PHP API, mysqli adapter behavior,
wire-protocol behavior, directory layout, WAL format, checkpoint record, redo
record, undo behavior, or recovery rule changes. This is an internal ownerless
classification reuse.

## Directory And Lifecycle Impact

No durable or transient files are added, removed, or moved. Runtime directory
lifecycle, shared-memory layout, page-log ordering, and checkpoint durability
are unchanged.

## Native Storage Impact

Native InnoDB page LSN installation, page latch release, transaction-level
dirty-page capture, page-version publication, and ownerless lock release
ordering are unchanged. The slice removes duplicate predicate work only after
the caller has already selected the transaction-deferred page path.

## Build And Performance Impact

The slice edits MariaDB-derived InnoDB MTR code, so the embedded MariaDB archive
must be rebuilt before production targets. The expected impact is a small
explicit-transaction hot-path reduction in ownerless hook and transaction-page
classification work for deferred page image capture. The change does not
reduce page-version count, page-log payload bytes, native-support proof volume,
redo/checkpoint work, or row/undo MTR commit cost.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive.
- Build production ownerless SQL and embedded performance targets.
- Run focused ownerless SQL selectors for explicit transaction deferred
  publication, multi-row visible-fast inserts, history WAL proof, and
  native-support WAL elision.
- Run reduced stats-enabled production attribution to confirm transaction page
  image publication, page-version, native-support, commit-visibility, and
  page-log append-session summaries remain coherent.
- Run a reduced stats-off production probe to keep branch throughput evidence
  comparable with the preceding performance slices.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Generic capture callers retain the existing hook-state and
  transaction-publish checks.
- Publish paths with an already computed transaction-publish result pass that
  result into the capture helper instead of recomputing it.
- The classified capture helper still validates transaction ownership and page
  LSN before recording a page image.
- Focused explicit-transaction and visible-fast SQL coverage continues to pass.
- Stats-enabled attribution preserves transaction page image publication,
  page-version, native-support/history-proof, commit-visibility, and page-log
  append-session invariants.

## Verification Results

Local production verification used `build/mariadb-embedded` with the documented
`MinSizeRel` cache and `build/php-embedded-prod` production targets.

Passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  explicit-transaction-visible-fast-commit`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-multi-row-insert-visible-fast-path`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-history-wal-proof`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  single-owner-native-support-page-wal-elision`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  active-reader-pressure`
- Filtered stats-enabled attribution probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`,
  `MYLITE_PERF_INSERT_ITERATIONS=500`,
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`,
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`, and
  `MYLITE_PERF_OWNERLESS_DATABASE_STATS=1`.
- Reduced stats-off production probes with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=2000`, and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`, followed by a shorter
  `1000`-insert confirmation sample.
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The stats-enabled sample preserved the explicit transaction shape:
`2` page versions per transaction, `2` native-support pages per transaction,
`1` history-proof rollback-segment page and `1` history-proof undo page per
transaction, `6` transaction image publishes, `4` transaction buffer publishes,
and `1` page-log append session begin/end pair around the actual transaction
page appends. Autocommit publication stayed at `3.008` page versions per
insert and `2.004` native-support pages per insert, with `1.000` history-proof
rollback-segment and undo pages per insert. The four-row bulk shape stayed at
`6.032` page versions per statement, `2.016` native-support pages published
per statement, and visible-fast commit without flush.

Reduced stats-off throughput samples were noisy and are not treated as a
stable win claim. Two `2000`-insert samples reported explicit transaction
ratios of `0.5582` and `0.8480`, autocommit ratios of `0.4139` and `0.4398`,
and four-row bulk row ratios of `0.3544` and `0.1627`. A shorter `1000`-insert
confirmation sample reported explicit transactions at `0.5571`, autocommit at
`0.6893`, four-row bulk rows at `0.3670`, ordinary active-runtime reconnect at
`1.015 ms`, and ownerless active-runtime reconnect at `0.838 ms`.

## Risks And Follow-Up

This slice assumes only callers that have just computed the existing
transaction-publish predicate pass `true` to the classified overload. It does
not remove the transaction-owned-page validation inside capture, so stale or
missing ownership still blocks image recording. Larger explicit-transaction
work remains in native row/undo MTR commit, history-proof publication, and
redo/checkpoint reconciliation.
