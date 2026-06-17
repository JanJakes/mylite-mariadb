# Ownerless Explicit Transaction History Proof

## Problem Statement

The explicit transaction visible-proof slice lets a proven ownerless explicit
`INSERT ... VALUES` transaction publish commit visibility without the later
`flush_unproven_statement` dirty-page fallback. The same focused probe still
reports `write_history_ownerless_flush_pages_per_transaction=2.000`, because
`trx_t::write_serialisation_history()` refuses the existing ownerless history
WAL proof for non-autocommit SQL transactions.

This slice extends the existing rollback-segment/undo history WAL proof to the
same explicit transaction shape that already has a conservative visible-fast
COMMIT proof.

## Source Findings

- Base: MariaDB 11.8.6 import `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `ownerless_history_wal_proof_allows_fast_path()` currently rejects a
  transaction unless `trx->auto_commit` is true or the SQL THD is in server
  autocommit mode. It then checks
  `mylite_ownerless_innodb_statement_visible_fast_path()`.
- `trx_t::write_serialisation_history()` sets
  `mylite_ownerless_history_proof_active`, records the expected rollback
  segment and undo-header page numbers, commits the mini-transaction, and skips
  `mylite_ownerless_innodb_flush_history_pages_to_lsn()` only when both pages
  were published by the ownerless page WAL without publish failure.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_can_elide_native_support_page()` refuses to elide pages
  that match the active history-proof rollback-segment or undo roles before it
  applies the explicit-transaction undo-page elision. That preserves active
  history pages for WAL publication.
- `packages/libmylite/src/database.cc`
  `ownerless_transaction_commit_allows_visible_fast_path()` marks `COMMIT`
  visible-fast only when the handle is still in an explicit transaction, has a
  local write, has the transaction visible-fast candidate, is not disqualified,
  and has no conservative dictionary refresh pending. Savepoint-controlled,
  locking-read, mixed-write, foreign-key target, and DDL shapes do not get the
  COMMIT marker.

## Design

Remove the autocommit-only prerequisite from
`ownerless_history_wal_proof_allows_fast_path()` and make the ownerless
statement visible-fast marker the proof boundary for both autocommit and
explicit SQL transaction commits.

This does not let arbitrary explicit transactions skip the history flush:

- direct/prepared explicit `INSERT ... VALUES` transactions get the marker only
  after the first-party transaction proof survives until `COMMIT`;
- savepoints, locking reads, non-visible-fast writes, DDL, dictionary
  conservative refresh, and foreign-key target inserts disqualify the marker;
- the existing history proof still has to publish both active history pages and
  still falls back to the native history flush if either page is not published
  or any publish failure occurs.

## Scope And Non-Goals

In scope:

- Proven ownerless explicit SQL transactions whose COMMIT already enters the
  visible-fast gate.
- The existing rollback-segment/undo history WAL proof and counters.
- Focused SQL coverage for prepared explicit inserts and savepoint negative
  coverage.

Out of scope:

- Mixed DML, `INSERT ... SELECT`, `UPDATE`, `DELETE`, `REPLACE`, DDL,
  foreign-key target inserts, savepoint-controlled transactions, and locking
  reads.
- Replacing history proof page contents with a smaller proof representation.
- Transaction-scoped append batching.
- Broader native redo/checkpoint reconciliation and crash matrices.

## Compatibility Impact

SQL results and MariaDB transaction semantics do not change. This only changes
whether MyLite can use the ownerless page-version WAL as the cross-process
visibility proof for history-list pages when COMMIT already has a conservative
visible-fast proof. Unsupported transaction shapes remain on the native flush
path.

## Directory And Native Storage Impact

No public API, directory layout, or durable file format changes are introduced.
History pages are published to the existing `mylite-concurrency.wal` page-log
records and checkpointed through the existing ownerless recovery path.

## Binary Size, License, And Dependency Impact

The slice changes a narrow MariaDB-derived predicate plus focused tests and
docs. It adds no dependency and has negligible binary-size impact.

## Test And Verification Plan

- Update the explicit transaction SQL selector to assert:
  - fast COMMIT publication remains present;
  - conservative COMMIT flush and unproven counters remain zero;
  - `write_history_ownerless_flush_pages` and exact fallback rounds are zero;
  - history-proof rollback-segment and undo pages are published and match the
    history-proof sample counters;
  - pre-commit undo native-support pages are still elided;
  - savepoint-controlled explicit transactions remain on the conservative
    unproven path.
- Run adjacent ownerless selectors covering autocommit history proof,
  native-support WAL elision, visible-fast inserts, and foreign-key blocking.
- Run reduced production performance probes with stats enabled and stats off.
- Run production build guards, formatting, and whitespace checks.

## Acceptance Criteria

- Proven explicit prepared insert transactions report zero ownerless
  write-history flush pages while preserving fast COMMIT publication.
- History-proof page counters prove both active history pages were published.
- Savepoint-controlled and otherwise unproven explicit transactions are not
  widened.
- Docs and compatibility matrix identify the narrowed proof boundary and
  remaining redo/checkpoint, append batching, and external stress gaps.

## Implementation And Verification Evidence

Implementation:

- `mariadb/storage/innobase/trx/trx0trx.cc` now makes the ownerless
  visible-fast statement marker the only history-proof eligibility boundary
  after read-only and dictionary-operation checks.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c` now requires
  the explicit transaction undo-WAL selector to report zero ownerless
  write-history flush pages, zero exact fallback rounds, and positive
  rollback-segment plus undo history-proof publication matching the sample
  counters.

Verification commands:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  explicit-transaction-undo-wal-elision`
- Adjacent selectors:
  `explicit-transaction-visible-fast-commit`,
  `single-owner-history-wal-proof`,
  `single-owner-native-support-page-wal-elision`,
  `single-owner-multi-row-insert-visible-fast-path`, and
  `insert-fk-fast-path-cache`.
- Production CTest hook/stress subset:
  `ctest --preset php-embedded-prod -R
  'libmylite\.embedded-ownerless-(trx|innodb-lock)-hooks|tools\.ownerless-transaction-stress-trace'
  --output-on-failure`
- Production CTest focused SQL selector subset:
  `ctest --preset php-embedded-prod -R
  'libmylite\.ownerless-single-owner-history-wal-proof|libmylite\.ownerless-single-owner-native-support-page-wal-elision|libmylite\.ownerless-single-owner-multi-row-insert-visible-fast-path|libmylite\.ownerless-insert-fk-fast-path-cache'
  --output-on-failure`
- Production guards:
  `tools/check-ci-production-builds`,
  `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`,
  `tools/require-cmake-release-build build/php-embedded-prod`,
  `cmake --build --preset format-check-prod`, and `git diff --check`.

Reduced production probe commands:

- stats-enabled counters:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=50 MYLITE_PERF_INSERT_ITERATIONS=80
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- stats-off throughput samples:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=50 MYLITE_PERF_INSERT_ITERATIONS=1000
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`

The stats-enabled sample reported:

- `commit_visibility_fast_per_transaction=1.000`
- `commit_visibility_flush_per_transaction=0.000`
- `commit_visibility_unproven_per_transaction=0.000`
- `native_support_published_history_proof_rseg_pages_per_transaction=1.000`
- `native_support_published_history_proof_undo_pages_per_transaction=1.000`
- `page_publish_transaction_image_published_per_transaction=3.000`
- `page_publish_transaction_buffer_published_per_transaction=9.000`
- `write_history_ownerless_flush_pages_per_transaction=0.000`

Stats-off 1000-row samples reported ownerless explicit transaction throughput
at `2274.01` and `2374.21` ops/s, with ordinary explicit transaction baselines
at `3822.45` and `3841.90` ops/s, ratios `0.5949` and `0.6180`. A 2000-row
sample reported `1792.34` ownerless ops/s versus `4263.10` ordinary ops/s,
ratio `0.4204`; treat this ratio as volatile host evidence rather than a
compatibility claim.

## Risks And Unresolved Questions

- This relies on the first-party COMMIT marker remaining the sole way for
  explicit transactions to enter the history proof. Future changes to statement
  classification must keep the visible-fast marker conservative.
- The proof still publishes full rollback-segment and undo page images; smaller
  history proof representation remains future work.
- Broader crash matrices and external MariaDB/RQG stress remain useful before
  claiming final ownerless completion.
