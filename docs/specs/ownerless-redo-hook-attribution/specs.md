# Ownerless Redo Hook Attribution

## Problem Statement

After the explicit transaction history-proof slice, the ownerless write path no
longer spends time flushing history pages for the proven prepared-insert
transaction shape. The current production stats-enabled probe still shows
nonzero ownerless cost in InnoDB mini-transaction commit and page-visible
publication. The detailed probe already prints aggregate redo hook counters,
but the compact `mylite_perf_summary_*` rows used for CI timing comparisons do
not expose redo enter, observe, reserve, written, and leave work per insert or
per explicit transaction.

This slice promotes existing first-party redo hook counters into compact
production summaries. It is diagnostic evidence for the next optimization, not
a durability or WAL-format change.

## Source Findings

- Base: MariaDB 11.8.6 import `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc::mtr_t::ownerless_redo_enter()`
  enters the MyLite redo-state hook before appending logged mini-transaction
  redo while ownerless hooks are active.
- `mtr_t::finish_writer()` calls
  `mylite_ownerless_innodb_redo_reserve()` before the MariaDB redo append and
  records the reserved LSN range for `ownerless_redo_leave()`.
- `mtr_t::ownerless_redo_leave()` waits for MariaDB redo up to the commit LSN,
  reports completed written redo ranges, and calls
  `mylite_ownerless_innodb_redo_leave()`.
- `packages/libmylite/src/database.cc` already records database perf counters
  for redo enter, observe, reserve, written, and leave hook calls plus elapsed
  time.
- `packages/libmylite/tests/embedded_performance_probe.c` already emits those
  counters in detailed `mylite_perf_ownerless_*` output, but the compact
  autocommit and explicit transaction summaries only expose checkpoint,
  page-log, page-write, commit-visibility, and InnoDB deep counters.

## Design

Add compact summary rows in the production embedded performance probe:

- autocommit per-insert redo hook calls and elapsed milliseconds for enter,
  observe, reserve, written, and leave;
- explicit transaction per-insert redo hook calls and elapsed milliseconds for
  the same hooks;
- explicit transaction per-transaction redo hook call counts for the same hooks,
  so a single large transaction can be distinguished from row-scaled work.

No hook behavior, lock ordering, checkpoint persistence, page-version WAL
records, native redo writes, SQL behavior, or public API changes.

## Compatibility Impact

No compatibility surface changes. The slice only changes performance probe
output.

## Directory And Native Storage Impact

No durable file, shared-memory, WAL, checkpoint, native tablespace, or runtime
lifecycle changes. Existing redo-state and checkpoint files are read and
written exactly as before.

## Binary Size, License, And Dependency Impact

The change touches one first-party test/probe source file plus docs. It adds no
dependency and has negligible binary-size impact.

## Test And Verification Plan

- Build the production embedded performance probe.
- Run a reduced stats-enabled production probe and verify the new summary keys
  appear for autocommit and explicit transaction insert phases.
- Run focused ownerless SQL selectors around history proof, native-support
  elision, visible-fast commit, and foreign-key blocking to ensure probe-only
  changes did not affect linked behavior.
- Run production build guards, format check, and whitespace checks.

## Acceptance Criteria

- Compact production probe output includes redo hook call/time summaries for
  ownerless autocommit inserts.
- Compact production probe output includes redo hook call/time summaries for
  ownerless explicit transaction inserts, both per insert and per transaction
  where transaction granularity matters.
- Docs identify this as attribution for the next performance slice, not as
  ownerless concurrency completion.

## Risks And Non-Goals

- This does not reduce runtime overhead by itself.
- This does not replace broader redo/checkpoint reconciliation or history-proof
  proof-volume work.
- This does not widen SQL-level concurrency claims or ownerless table-lock
  coverage.

## Implementation And Verification Evidence

Implementation:

- `packages/libmylite/tests/embedded_performance_probe.c` now emits compact
  redo hook summaries from the existing database perf counters:
  - `mylite_perf_summary_ownerless_autocommit_redo_*_per_insert`;
  - `mylite_perf_summary_ownerless_insert_txn_redo_*_per_insert`;
  - `mylite_perf_summary_ownerless_insert_txn_redo_*_per_transaction`.

Verification commands:

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- stats-enabled reduced production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=50 MYLITE_PERF_INSERT_ITERATIONS=200
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- direct focused selectors:
  `explicit-transaction-undo-wal-elision`,
  `explicit-transaction-visible-fast-commit`,
  `single-owner-history-wal-proof`,
  `single-owner-native-support-page-wal-elision`,
  `single-owner-multi-row-insert-visible-fast-path`, and
  `insert-fk-fast-path-cache`.
- production CTest focused SQL selector subset:
  `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-single-owner-history-wal-proof|libmylite\.ownerless-single-owner-native-support-page-wal-elision|libmylite\.ownerless-single-owner-multi-row-insert-visible-fast-path|libmylite\.ownerless-insert-fk-fast-path-cache'
  --output-on-failure`
- production guards:
  `tools/check-ci-production-builds`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `cmake --build --preset format-check-prod`, and `git diff --check`.

The stats-enabled sample reported these new summary rows:

- autocommit: `redo_enter_calls_per_insert=3.000`,
  `redo_reserve_calls_per_insert=3.000`,
  `redo_written_calls_per_insert=3.000`,
  `redo_leave_calls_per_insert=3.000`,
  and `redo_leave_ms_per_insert=0.022`;
- explicit transaction: `redo_enter_calls_per_insert=2.005`,
  `redo_reserve_calls_per_insert=2.005`,
  `redo_written_calls_per_insert=2.005`,
  `redo_leave_calls_per_insert=2.005`,
  and `redo_leave_ms_per_insert=0.016`;
- explicit transaction per transaction: `redo_enter_calls_per_transaction=401.000`,
  `redo_reserve_calls_per_transaction=401.000`,
  `redo_written_calls_per_transaction=401.000`,
  `redo_leave_calls_per_transaction=401.000`,
  and `redo_leave_ms_per_transaction=3.159`.

In the same sample, autocommit page-write commit-log publish was
`0.095 ms/insert`, page-write publish was `0.094 ms/insert`, page-log append
was `0.058 ms/insert`, checkpoint update was `0.024 ms/insert`, and page-write
commit-log redo-leave was `0.037 ms/insert`. This keeps the next runtime
optimization focused on page publication/commit-publish and native
redo/checkpoint reconciliation, while tracking redo hook overhead separately.
