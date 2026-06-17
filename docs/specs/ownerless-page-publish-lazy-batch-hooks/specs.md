# Ownerless Page-Publish Lazy Batch Hooks

## Problem

Ownerless page-version publication can batch page-log append session setup
through `mylite_ownerless_innodb_begin_page_publish_batch()` and
`mylite_ownerless_innodb_end_page_publish_batch()`. The current MTR publish
loops call that begin/end hook pair as soon as a publish pass is possible.
Explicit-transaction and transaction-deferred page paths can then run a pass
that only records dirty transaction pages and captures their images, without
publishing an immediate page-version record from that MTR loop.

That deferred-only shape still pays the ownerless batch hook atomics and
callback hop. Current production profiling points remaining write-path work at
native page-publication, page-log append, and transaction commit work, so
publish loops should avoid diagnostics and hook plumbing when they do not
reach an immediate page publish.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_writes_publish_list()`
  publishes an already collected modified-page list after dirty MTR commit
  work. It may defer pages to transaction-level publication when
  `ownerless_page_write_uses_transaction_release()` is true.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_page_writes_publish()`
  scans the MTR memo for modified pages and applies the same immediate versus
  transaction-deferred publish split.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::commit_log()` has a
  no-dirty commit-log release loop that can publish modified pages immediately
  or capture transaction-deferred page images while releasing MTR latches.
- `packages/libmylite/src/database.cc::ownerless_innodb_page_publish_batch_begin_hook()`
  only prepares thread-local page-log append batching. A page-log append
  session is actually opened later from `append_ownerless_page_version()` when
  an immediate page-version publish reaches the MyLite hook.
- `packages/libmylite/src/database.cc::ownerless_innodb_page_publish_batch_end_hook()`
  releases an active append session unless the current statement explicitly
  defers the session for visible-fast batching.

## Design

Start the page-publish batch lazily inside the existing publish loops. Each
loop keeps a local `batch_started` flag and calls a small helper before the
first immediate `ownerless_page_write_publish()` call. The loop calls
`mylite_ownerless_innodb_end_page_publish_batch()` only when that helper started
the batch.

Transaction-deferred pages still call
`ownerless_page_write_note_dirty_transaction_page()` and
`ownerless_page_write_capture_dirty_transaction_page()` in the same position
and with the same latches held. Immediate page publishes still share one batch
for the pass, preserving existing append-session batching.

## Compatibility Impact

No SQL semantics, public C API, PHP API, mysqli adapter behavior,
wire-protocol behavior, directory layout, WAL format, checkpoint record, redo
record, undo behavior, or recovery rule changes. This is an internal hook
overhead reduction.

## Directory And Lifecycle Impact

No durable or transient files are added, removed, or moved. Runtime directory
lifecycle, shared-memory layout, checkpoint durability, and page-log
payload-before-header ordering are unchanged.

## Native Storage Impact

Native InnoDB MTR commit ordering is unchanged. Page LSN installation,
transaction-deferred dirty-page capture, page-version publication,
history-proof marking, page-write lock release, and tablespace-write release
keep their existing order. The slice changes only whether the MyLite
page-publish batch hook is called for publish passes that never call the
immediate page publish hook.

## Build And Performance Impact

The slice edits MariaDB-derived InnoDB MTR code, so the embedded MariaDB archive
must be rebuilt before production targets. The expected performance impact is
small but production-visible: transaction-deferred publish passes avoid two
ownerless hook-entry calls and their relaxed/acquire atomics when no immediate
page-version publish occurs. The change does not reduce page-version count,
native-support history-proof count, page-log payload bytes, redo/checkpoint
work, or row/undo MTR commit cost.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive.
- Build production ownerless SQL and embedded performance targets.
- Run focused ownerless SQL selectors for explicit transaction deferred
  publication, multi-row visible-fast inserts, history WAL proof, and
  native-support WAL elision.
- Run reduced stats-enabled production attribution to confirm page-version,
  native-support, commit-visibility, page-log append-session, and page-write
  summaries remain coherent.
- Run a reduced stats-off production probe to keep branch throughput evidence
  comparable with the prior performance slices.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- `ownerless_page_writes_publish_list()`,
  `ownerless_page_writes_publish()`, and the no-dirty `commit_log()` publish
  loop open page-publish batches only before an immediate
  `ownerless_page_write_publish()` call.
- Deferred-only transaction publish passes do not call the page-publish batch
  begin/end hooks.
- Immediate publish passes still preserve one batch around all immediate page
  publishes in that loop.
- Focused explicit-transaction and visible-fast SQL coverage continues to pass.
- Stats-enabled production attribution preserves page-version publication,
  native-support/history-proof publication, commit-visibility, and page-log
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
- Reduced stats-off production probe with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=100`,
  `MYLITE_PERF_INSERT_ITERATIONS=2000`, and
  `MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=4`.
- `tools/check-ci-production-builds`
- `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The stats-enabled sample preserved the expected page publication shape:
explicit transaction publication emitted `2` page versions per transaction,
`2` native-support pages per transaction, `1` history-proof rollback-segment
page and `1` history-proof undo page per transaction, and `1` page-log append
session begin/end pair around the actual transaction page appends. Autocommit
publication stayed at `3.008` page versions per insert, `2.004`
native-support pages published per insert, and `1.000` history-proof
rollback-segment and undo pages per insert. The four-row bulk shape stayed at
`6.032` page versions per statement, `2.016` native-support pages published
per statement, and one page-log append session begin/end pair per statement.

The reduced stats-off sample reported ordinary explicit transactions at
`4085.03 ops/s`, ownerless explicit transactions at `2628.78 ops/s`, ratio
`0.6435`; ordinary autocommit at `3316.55 ops/s`, ownerless autocommit at
`1511.79 ops/s`, ratio `0.4558`; ordinary four-row bulk rows at
`12065.41 rows/s`, ownerless four-row bulk rows at `5095.06 rows/s`, ratio
`0.4223`; ordinary active-runtime reconnect at `0.824 ms` and ownerless
active-runtime reconnect at `1.271 ms`.

## Risks And Follow-Up

This slice intentionally does not attempt native-support elision, history-proof
replacement, WAL format changes, or redo/checkpoint reconciliation. Those remain
the larger correctness-sensitive write-throughput targets. The lazy-batch
change assumes an active page-log append session remains live across MTRs only
under the existing statement-deferred append-batch guard; the verification plan
therefore includes the multi-row visible-fast selector that asserts session
begin/end behavior.
