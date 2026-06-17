# Ownerless Explicit Transaction Undo Elision

## Problem

The production embedded performance probe now exposes prepared single-row
inserts inside one explicit ownerless transaction as a remaining performance
gate. The stats-enabled sample in
`ownerless-explicit-transaction-publish-attribution` reported about one
native-support undo page-version publication and one page-log append session
per inserted row, while the transaction's user data/index pages were already
deferred and published at commit.

Those per-row undo page-version records are not SQL-visible before the
transaction commits. The final commit still writes MariaDB's serialization
history. If a future explicit-transaction path is proven for visible-fast
commit publication, the active history-proof rollback-segment and undo pages
must still be published or otherwise proved before skipping the exact native
history flush. At the time this slice landed, the measured prepared explicit
transaction remained unproven for visible-fast commit publication and used the
conservative native flush fallback. The follow-up
`ownerless-explicit-transaction-visible-proof` slice carries a narrower
transaction-scoped proof for eligible `INSERT ... VALUES` transactions; this
slice remains the undo-WAL reduction that made that later proof cheaper. This
slice narrows native-support WAL publication for explicit transactions without
changing transaction visibility, redo, or crash recovery semantics.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/trx/trx0rec.cc`
  `trx_undo_report_row_operation()` writes an undo record for each prepared
  insert row in the measured explicit-transaction probe. The bulk
  already-covered branch is not selected for this statement shape.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `mtr_t::commit_log()` publishes ownerless page versions in the no-dirty
  mini-transaction branch after redo release. In explicit transactions,
  `ownerless_page_write_uses_transaction_release()` already defers user
  data/index pages to the transaction boundary, but undo pages are native
  support pages and still publish at mini-transaction scope.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc`
  `ownerless_page_write_can_elide_native_support_page()` currently elides
  native-support pages only for autocommit visible-fast statements in the
  transaction rollback segment's own space, and refuses active history-proof
  rollback-segment/undo pages.
- `mariadb/storage/innobase/trx/trx0trx.cc`
  `trx_t::write_serialisation_history()` marks
  `mylite_ownerless_history_proof_active` around the commit mini-transaction
  and requires the rollback-segment and undo history-proof page images before
  skipping the exact native history flush.
- `trx_t::commit_in_memory()` publishes transaction-deferred page images before
  deciding whether visibility can use the fast page-version path or must use
  the conservative native dirty-page flush bridge.

## Design

Extend `ownerless_page_write_can_elide_native_support_page()` with a narrow
explicit-transaction branch:

- the page must be a native-support undo page in the transaction's own rollback
  segment space;
- the InnoDB transaction must be a non-read-only, non-dictionary ownerless
  transaction;
- the page must not be one of the active history-proof pages for the commit
  mini-transaction;
- the transaction must not be SQL autocommit or `auto_commit`.

The existing autocommit visible-fast branch remains unchanged. If an explicit
transaction can use visible-only publication, active history-proof
rollback-segment and undo pages still bypass this elision at commit. If the
transaction cannot use visible-only publication, as in the current prepared
single-row insert probe shape, the existing commit bridge still flushes native
dirty pages before publishing visibility.

No new durable format, public API, shared-memory layout, or page-log record
type is added.

## Scope And Non-Goals

In scope:

- explicit ownerless transactions that write rollback-segment undo pages;
- MariaDB-derived `mtr0mtr.cc` page-publish classification;
- focused SQL coverage and production probe summaries proving elision and
  the conservative commit fallback.

Out of scope:

- eliding rollback-segment history-proof pages at commit;
- eliding generic `FIL_PAGE_TYPE_SYS`, `FIL_PAGE_TYPE_TRX_SYS`, space
  metadata, user data/index, BLOB, or dictionary pages;
- holding the page-log append lock across an application transaction;
- changing SQL semantics, transaction isolation, WAL format, native redo, or
  checkpoint/reclaim rules.

## Compatibility Impact

No SQL, C API, PHP API, wire-protocol, storage-format, or directory-layout
behavior changes. Committed explicit transactions remain visible through the
same ownerless commit bridge, and non-fast explicit transaction commits retain
the conservative native flush fallback.

## Native Storage Impact

MariaDB still writes native undo records, redo, and serialization history. The
optimization skips only ownerless page-version WAL records for pre-commit
rollback-segment undo pages that are not active history-proof pages. When the
exact native history flush is skipped, active history-proof pages remain
protected by the existing proof guard. When the transaction is not proven for
visible-fast publication, the commit bridge keeps the conservative native flush
fallback.

## Binary Size Impact

No new dependency. The implementation is a small MariaDB-derived conditional
and focused first-party tests/docs.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing `mtr0mtr.cc`.
- Build production `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe`.
- Add and run a focused SQL selector that:
  - executes many prepared inserts in one explicit ownerless transaction;
  - asserts native-support undo pages are elided during the transaction;
  - asserts the current prepared transaction shape uses the conservative
    commit flush fallback without publish failure;
  - verifies committed rows through same-handle, reopened ownerless, and
    forced-`.shm` native reopen paths.
- Run adjacent native-support/history-proof selectors.
- Run a reduced stats-enabled production performance probe and confirm explicit
  transaction page-version/append volume drops while the then-current prepared
  transaction still commits through the conservative flush fallback without
  publish failure.
- Run production build guards, format check, and whitespace checks.

## Acceptance Criteria

- Explicit transaction inserts elide pre-commit native-support undo page WAL
  records.
- The then-current measured prepared explicit transaction commits through the
  conservative flush fallback without page-publish failure.
- Active history-proof rollback-segment and undo pages remain blocked from this
  elision before any visible-fast explicit transaction path can skip
  the exact native history flush.
- Focused ownerless correctness coverage for explicit transaction reopen and
  forced shared-memory rebuild passes.
- Probe output records the reduced explicit transaction page-version and
  page-log append counts.
- Docs state the remaining limitations and do not claim broad native-support
  elision.

## Verification Results

Completed on 2026-06-17 with `build/mariadb-embedded` at `MinSizeRel` and
`build/php-embedded-prod` at `Release`:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `mylite_ownerless_cross_process_sql_test
  explicit-transaction-undo-wal-elision`
- adjacent selectors:
  `single-owner-history-wal-proof`,
  `single-owner-native-support-page-wal-elision`, and
  `single-owner-multi-row-insert-visible-fast-path`
- stats-enabled reduced production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=50 MYLITE_PERF_INSERT_ITERATIONS=80
  MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- stats-off reduced production probe:
  `MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
  MYLITE_PERF_SELECT_ITERATIONS=50 MYLITE_PERF_INSERT_ITERATIONS=200
  build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe`
- `tools/check-ci-production-builds`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `cmake --build --preset format-check-prod`
- `git diff --check`

The stats-enabled 80-row sample reported
`mylite_perf_summary_ownerless_insert_txn_page_versions_per_insert=0.050`,
`native_support_elided_undo_pages_per_insert=1.012`,
`native_support_published_undo_pages_per_insert=0.000`,
`page_log_session_begin_calls_per_insert=0.025`,
`page_log_append_ms_per_insert=0.008`,
`commit_visibility_flush_per_transaction=1.000`,
`commit_visibility_unproven_per_transaction=1.000`, and
`write_history_ownerless_flush_pages_per_transaction=2.000`. The stats-off
200-row sample reported explicit transaction throughput at `1411.97` ownerless
ops/s versus `2272.97` ordinary ops/s, ratio `0.6212`; the same sample reported
ownerless autocommit ratio `0.4928` and bulk-row ratio `0.3922`.

## Risks And Follow-Up

- This relies on the invariant that pre-commit explicit-transaction undo pages
  are not needed by peers until the transaction-visible commit boundary. The
  focused test covers ordinary visibility and rebuild, but broader randomized
  transaction stress remains useful follow-up.
- The page-write commit-log loop and redo release still run per row. This slice
  reduces page-version WAL append work, not all explicit transaction overhead.
- Future work may consider transaction-scoped append batching only with a
  design that does not hold the global append lock across application idle
  time.
