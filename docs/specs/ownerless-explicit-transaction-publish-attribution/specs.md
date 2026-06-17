# Ownerless Explicit Transaction Publish Attribution

## Problem

The production embedded performance probe shows ownerless reads close to the
ordinary embedded path, while prepared single-row inserts inside one explicit
transaction can lag ordinary inserts substantially. Recent stats-enabled samples
show the final SQL `COMMIT` is not the only cost center: per-row undo-report
mini-transactions, page-write commit-log handling, and page-log append work are
visible during each prepared statement step.

The probe already emits detailed compact summaries for ownerless autocommit and
bulk autocommit writes. Explicit transaction inserts only had raw counter rows,
which made CI timing summaries and local comparisons harder to read. This slice
adds compact transaction-attribution rows without changing ownerless storage
behavior.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0rec.cc` implements
  `trx_undo_report_row_operation()`. The single-row explicit transaction probe
  still reports one undo record path per inserted row; the bulk already-covered
  branch is not selected for this prepared statement shape.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc` implements `mtr_t::commit_log()`.
  Ownerless no-dirty mini-transaction commits still release memo state and can
  publish native-support page versions, so per-row commit-log timing matters
  even before the final SQL transaction commit.
- `mariadb/storage/innobase/trx/trx0trx.cc` publishes ownerless transaction
  visibility from `trx_t::commit_in_memory()` and flushes deferred ownerless
  page-write state when needed at SQL transaction commit.
- `packages/libmylite/src/ownerless_page_log.cc` owns direct and append-session
  page-log appends. The current visible-fast statement batching releases an
  append session at a statement boundary, not across an application transaction.
- `packages/libmylite/tests/embedded_performance_probe.c` already reads the
  relevant page-publish, commit-visibility, page-write, page-log, and deep
  InnoDB counters for stats-enabled probe runs.

## Design

Add a compact explicit transaction summary to the production performance probe
when `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1` is set:

- store ordinary explicit-transaction deep InnoDB counters before the ordinary
  autocommit probe resets them;
- store ownerless explicit-transaction deep InnoDB counters before the
  ownerless autocommit probe resets them;
- emit ownerless explicit-transaction page-version, native-support,
  page-publish-hook, page-log append, page-write commit-log, commit-visibility,
  transaction-page publish, undo-report, undo-cache, and write-history summary
  rows;
- emit ordinary, ownerless, and ownerless-minus-ordinary rows for the most
  important deep insert/undo counters.

The summary is deliberately diagnostic only. It does not hold the page-log
append lock across a user transaction, change page-version publication, or
alter MariaDB transaction semantics.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, storage format, or durable directory
layout behavior changes. The production probe output gains additional
`mylite_perf_summary_*` rows under an opt-in stats flag.

## Native Storage Impact

No native storage behavior changes. The rows attribute existing ownerless
InnoDB page-version publication and MariaDB undo/MTR work.

## Binary Size Impact

No new dependency. The change is limited to a test/probe executable and docs.

## Test Plan

- Build `mylite_embedded_performance_probe` with the production PHP embedded
  preset.
- Run a reduced stats-enabled production probe and verify the new
  `mylite_perf_summary_ownerless_insert_txn_*` keys are emitted.
- Run a reduced stats-off production probe and verify the new summary keys
  remain gated by `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS`.
- Run production build-type guards, format check, and whitespace checks.

## Acceptance Criteria

- Stats-enabled production probe output includes compact explicit transaction
  summaries for page versions, page-log append sessions, page-write commit-log
  cost, commit visibility, undo-report MTR cost, and ownerless-minus-ordinary
  row-insert/undo deltas.
- Stats-off production probe output keeps its existing compact shape.
- The slice documents that this is attribution only, not a performance
  optimization or a new ownerless compatibility claim.

## Risks And Follow-Up

- Stats-enabled timings carry instrumentation overhead, so they identify cost
  shape rather than final throughput.
- A transaction-wide page-log append session might reduce single-process probe
  cost, but doing so naively would hold a global append lock across arbitrary
  application transaction idle time. Any future optimization needs a separate
  correctness and concurrency design.
- The next optimization candidate should target the largest proven explicit
  transaction cost while preserving peer write concurrency and crash recovery.

## Verification Results

Local verification on 2026-06-17 used production embedded builds:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe` passed.
- A reduced stats-enabled production probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=50`,
  `MYLITE_PERF_INSERT_ITERATIONS=80`, and
  `MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`. It emitted the new compact
  explicit transaction summaries, including
  `mylite_perf_summary_ownerless_insert_txn_page_versions_per_insert=1.062`,
  `mylite_perf_summary_ownerless_insert_txn_page_log_session_begin_calls_per_insert=1.012`,
  `mylite_perf_summary_ownerless_insert_txn_page_write_commit_log_ms_per_insert=0.074`,
  `mylite_perf_summary_ownerless_insert_txn_commit_visibility_total_ms_per_transaction=1.749`,
  `mylite_perf_summary_ownerless_insert_txn_undo_report_mtr_commit_ms_per_insert=0.064`,
  `mylite_perf_summary_ownerless_minus_ordinary_insert_txn_undo_report_mtr_commit_ms_per_insert=0.062`,
  and
  `mylite_perf_summary_ownerless_insert_txn_write_history_ownerless_flush_pages_per_transaction=2.000`.
  The same reduced run reported
  `mylite_perf_summary_ownerless_insert_txn_ratio=0.7581`.
- A reduced stats-off production probe passed with
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1`,
  `MYLITE_PERF_SELECT_ITERATIONS=20`, and
  `MYLITE_PERF_INSERT_ITERATIONS=20`; it reported
  `mylite_perf_summary_ownerless_insert_txn_ratio=0.3871` and did not emit
  the new stats-gated transaction summary keys.
- `tools/check-ci-production-builds`, `tools/require-cmake-build-type
  MinSizeRel build/mariadb-embedded`, `tools/require-cmake-release-build
  build/php-embedded-prod`, `cmake --build --preset format-check-prod`, and
  `git diff --check` passed. `ctest --preset php-embedded-prod -N` did not
  list a CTest wrapper for the standalone performance probe.
