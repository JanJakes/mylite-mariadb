# Ownerless Default-Checked Bulk Insert

## Problem

The ownerless PHP/WordPress-oriented write profile is still dominated by
per-row InnoDB write work after page-log append batching and ownerless
page-write tracking elision. A reduced production 100-row ownerless row-list
insert after `f24ff208` showed the remaining hot buckets inside
`row_insert_for_mysql()`, especially `trx_undo_report_row_operation()` and its
mini-transaction commit work.

MariaDB already has an empty-table bulk insert path that writes one
`TRX_UNDO_EMPTY` record and buffers the row list instead of writing per-row
undo records. The row-level gate in `row0ins.cc` requires both
`unique_checks=0` and `foreign_key_checks=0`, even for a table shape with only
the clustered primary key and no foreign-key relationships. MyLite ownerless
workloads run with default SQL settings, so the measured primary-key-only
row-list probe continued to pay per-row undo.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_insert.cc::mysql_insert()` calls
  `handler::ha_start_bulk_insert()` only for multi-row `INSERT ... VALUES`
  statements that pass SQL-layer checks for same-table trigger/function reads.
- `mariadb/storage/innobase/row/row0ins.cc::row_ins_clust_index_entry_low()`
  starts the InnoDB empty-table bulk path only when the clustered root page is
  empty, the statement is not duplicate-handling DML, no dictionary/native
  online-DDL special case applies, the table has no row locks and is not
  temporary/versioned/spatial, and SQL `unique_checks` and
  `foreign_key_checks` are both disabled.
- `mariadb/storage/innobase/include/trx0trx.h::trx_t::use_bulk_buffer()` and
  `trx_t::is_bulk_insert()` enforce the same check-disabled assumption after
  the bulk buffer is created.
- `mariadb/storage/innobase/row/row0merge.cc::row_merge_bulk_t` sorts and
  duplicate-checks unique indexes before writing buffered rows and rolls the
  transaction back through `trx_t::bulk_rollback_low()` when buffered bulk
  apply fails.

## Design

Add a MyLite-owned, ownerless-only relaxation for the existing InnoDB
empty-table bulk insert path. The relaxation is intentionally narrower than
MariaDB's general bulk policy:

- ownerless InnoDB hooks must be active;
- the transaction must be autocommit;
- the table must have exactly one InnoDB index, the clustered index;
- the table must have no outgoing or incoming foreign-key relationships when
  `foreign_key_checks` is enabled;
- all existing row0ins gates for empty root page, no duplicate-handling DML,
  no dictionary operation, no native online DDL, no temporary/versioned/spatial
  table, no existing row locks, and SQL-layer bulk eligibility still apply.

`trx_t::bulk_insert_checks_allow_buffer()` preserves the upstream disabled-check
case and admits only the new ownerless default-checked table shape. The helper
is used by the clustered insert gate, `trx_t::is_bulk_insert()`, and
`trx_t::use_bulk_buffer()` so statement-boundary bulk apply and cleanup see the
same eligibility decision that created the buffer.

Append a deep performance counter for ownerless default-checked bulk starts.
Keep previous deep-counter indexes stable.

## Compatibility Impact

For the bounded ownerless table shape, SQL results are intended to match
ordinary MariaDB/InnoDB behavior: successful row-list inserts commit the same
rows, duplicate-key failures reject the statement, and the failed row list
leaves no partial rows. Ordinary non-ownerless embedded opens keep upstream
MariaDB's stricter `unique_checks=0`/`foreign_key_checks=0` bulk requirement.

This does not broaden ownerless support for secondary indexes, unique secondary
checks, foreign keys, explicit transactions, triggers/functions that read the
target table, `INSERT ... ON DUPLICATE KEY UPDATE`, `REPLACE`, `INSERT ...
SELECT`, DDL, or non-InnoDB engines.

## Directory And Lifecycle Impact

No new durable files, shared-memory records, WAL record formats, checkpoint
formats, or directory lifecycle states are introduced.

## Native Storage Impact

The slice reuses MariaDB's existing InnoDB bulk buffer, `TRX_UNDO_EMPTY` undo
record, duplicate checking, `bulk_insert_apply()`, and `bulk_rollback_low()`
paths. It reduces per-row undo for one ownerless autocommit row-list shape but
does not skip redo, page-version WAL, page-visible checkpoint publication,
commit visibility, or ownerless page-write release.

## Test And Verification Plan

- Rebuild the MariaDB embedded archive after editing InnoDB sources.
- Build production `mylite_ownerless_cross_process_sql_test` and
  `mylite_embedded_performance_probe`.
- Extend `ownerless-single-owner-multi-row-insert-visible-fast-path` to assert
  the default-checked bulk-start counter is positive, duplicate-key row-list
  failure leaves no partial rows, and a later valid row-list insert succeeds.
- Run the focused production selector plus the existing ownerless visible-fast,
  native-support, history-proof, FK-cache, and uncommitted-peer subset.
- Run the hook-build ownerless subset and ownerless stress preset.
- Run reduced stats-enabled and stats-off production probes with 100-row bulk
  statements and record the default-checked bulk-start counter and throughput
  shape.
- Run production build guards, format check, and `git diff --check`.

## Acceptance Criteria

- Ownerless default-check bulk starts are counted for the primary-key-only
  multi-row fast-path test.
- Duplicate-key failure in the new bounded row-list shape rolls the full
  statement back.
- Existing visible-fast publication, page-version, history-proof,
  native-support, page-write, and commit-visibility assertions still pass.
- Ordinary/non-ownerless behavior is unchanged.
- Docs do not claim ownerless concurrency completion or broad SQL-level bulk
  coverage.

## Verification Results

Local production and hook verification passed:

- `tools/mariadb-embedded-build build`
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test mylite_embedded_performance_probe`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-single-owner-multi-row-insert-visible-fast-path$'
  --output-on-failure` passed in `4.53 sec`.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-(primitives|single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|insert-fk-fast-path-cache|uncommitted-peer-hidden)$'
  --parallel 2 --output-on-failure` passed `6/6` in `14.87 sec`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test mylite_ownerless_primitives_test`
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-(primitives|single-owner-(multi-row-insert-visible-fast-path|history-wal-proof|native-support-page-wal-elision)|negative-proof|history-proof-publish-failure-fallback)$'
  --parallel 2 --output-on-failure` passed `6/6` in `10.23 sec`.
- `cmake --build --preset ownerless-stress`
- `ctest --preset ownerless-stress --output-on-failure` passed `12/12`
  in `577.83 sec`.
- `tools/check-ci-production-builds`
- `tools/require-cmake-build-type MinSizeRel build/mariadb-embedded`
- `tools/require-cmake-release-build build/php-embedded-prod`
- `tools/require-cmake-release-build build/ownerless-test-hooks`
- `tools/require-cmake-release-build build/ownerless-stress`
- `cmake --build --preset format-check-prod`
- `git diff --check`

A reduced stats-enabled 100-row bulk probe with one bulk statement confirmed
the new fast path and unchanged ownerless publication shape:

- `mylite_perf_ownerless_insert_autocommit_bulk_innodb_deep_row_ins_clust_low_ownerless_default_checked_bulk=1`
- `mylite_perf_summary_ownerless_autocommit_bulk_default_checked_bulk_starts_per_statement=1.000`
- `mylite_perf_summary_ownerless_autocommit_bulk_page_versions_per_statement=2.000`
- `mylite_perf_summary_ownerless_autocommit_bulk_page_log_append_calls_per_statement=8.000`
- `mylite_perf_summary_ownerless_autocommit_bulk_page_write_commit_log_redo_leave_ms_per_statement=0.067`

A stats-off production-style probe with 500 insert iterations and 100 rows per
bulk statement reported:

- ordinary warm open/close `139.324 ms`, ownerless warm open/close
  `246.690 ms`;
- ordinary active-runtime reconnect `0.670 ms`, ownerless active-runtime
  reconnect `0.786 ms`;
- ordinary autocommit insert `3641.38 ops/s`, ownerless autocommit insert
  `1960.43 ops/s`, ratio `0.5384`;
- ordinary 100-row bulk insert `106564.12 rows/s`, ownerless 100-row bulk
  insert `24211.61 rows/s`, ratio `0.2272`.

## Risks And Follow-Up

This is a bounded performance slice, not a general SQL bulk-insert redesign.
Secondary indexes, foreign-key tables, explicit transactions, broader DML, and
larger PHPUnit timing analysis remain future work. The next performance slice
should re-profile PHP/WordPress production timings after this optimization
lands, because the dominant bucket may move from undo-report MTR commit back to
page-version publication or native redo/checkpoint reconciliation.
