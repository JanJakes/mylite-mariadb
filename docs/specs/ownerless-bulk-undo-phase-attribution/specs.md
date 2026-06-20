# Ownerless Bulk Undo Phase Attribution

## Problem

The 2048-row ownerless bulk path now preserves append batching and uses an
exact transaction page-membership cache, but later non-empty-table statements
still spend most ownerless overhead inside native row insert and row-level undo.
The existing bulk first/remaining summary reports row insert, clustered insert,
undo-report total, undo-report MTR commit, and undo-report call counts, but it
does not phase-split the deeper undo-report or B-tree lock/undo subcounters
that are already collected by the probe.

Without those phase rows, the next native undo optimization would still have to
infer whether later-statement cost belongs to persistent undo assignment,
record insertion, page allocation, B-tree lock/undo glue, or MTR commit.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `packages/libmylite/tests/embedded_performance_probe.c`
  captures ordinary and ownerless deep InnoDB counters for the aggregate bulk
  loop and also snapshots the first bulk statement.
- The same probe subtracts the first-statement snapshot from total deep
  counters to derive a remaining-statements snapshot in
  `emit_bulk_deep_phase_comparison_summary()`.
- `emit_raw_innodb_deep_perf()` already prints raw counters for
  `trx_undo_report_prelude`, `trx_undo_report_assign_persistent`,
  `trx_undo_report_insert_record`, `trx_undo_report_add_page`, and the
  `row_ins_btr_lock_undo_*` subphases, but the compact first/remaining bulk
  summary currently omits those rows.
- MariaDB's default-checked bulk insert path remains limited to the first
  empty-table insert statement; this slice must not reinterpret the later
  row-level undo path as covered by `TRX_UNDO_EMPTY`.

## Design

Extend `emit_bulk_deep_comparison_summary_for_phase()` to emit first,
remaining, and aggregate rows for existing deep counters:

- B-tree lock/undo subphase timings and counts;
- undo-report prelude, persistent/temp assignment, insert/modify record,
  success bookkeeping, add-page timing, and error/success counts.

The metric names use the existing bulk phase naming scheme:
`mylite_perf_summary_{ordinary,ownerless,ownerless_minus_ordinary}_autocommit_bulk_{phase}_{metric}_per_{row,statement}`.
Existing aggregate metric names remain unchanged.

## Compatibility Impact

None. This is diagnostic-only performance attribution in the embedded
performance probe. It does not change SQL behavior, public API behavior,
directory layout, WAL records, page-version policy, native InnoDB storage, or
ownerless locking.

## Directory And Lifecycle Impact

None. The probe still creates and removes temporary MyLite database directories
through the existing harness.

## Native Storage Impact

None. Native row insert, undo, redo, rollback, and savepoint behavior remain
unchanged.

## Build And Performance Impact

The implementation changes first-party test/probe code only. It does not
require a MariaDB embedded archive rebuild. Stats-enabled performance probe
output grows by additional summary rows; stats-off throughput runs are
unchanged.

## Verification Plan

- Build `mylite_embedded_performance_probe` with `php-embedded-prod`.
- Run a reduced stats-enabled 2048-row production probe and confirm the new
  `remaining_trx_undo_report_assign_persistent`,
  `remaining_trx_undo_report_insert_record`,
  `remaining_trx_undo_report_add_page`, and
  `remaining_row_ins_btr_lock_undo_undo_report` rows are emitted.
- Run a reduced one-statement probe and confirm remaining rows are zero rather
  than reusing aggregate counters.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Acceptance Criteria

- Existing aggregate bulk summary names remain present.
- First and remaining summaries include the deeper undo-report and B-tree
  lock/undo rows.
- One-statement probes emit zero remaining-statement averages.
- Docs continue to state that non-empty-table row lists preserve row-level
  undo semantics; no compatibility claim is broadened.

## Verification Results

Passed:

- `cmake --build --preset php-embedded-prod --target
  mylite_embedded_performance_probe`
- `tools/check-ci-production-builds`
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-primitives$' --output-on-failure`
- `cmake --build --preset format-check-prod`
- `git diff --check`

Reduced stats-enabled 2048-row production probe:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=20
MYLITE_PERF_INSERT_ITERATIONS=20480
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=2048
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The probe emitted the new phase rows. Later non-empty ownerless statements
reported:

- `45.959 ms` remaining row insert per statement;
- `25.338 ms` remaining clustered optimistic lock/undo per statement;
- `20.857 ms` remaining B-tree lock/undo undo-report subphase per statement;
- `20.606 ms` remaining undo report per statement;
- `13.523 ms` remaining undo-report MTR commit per statement;
- `4.671 ms` remaining persistent undo assignment per statement;
- `1.248 ms` remaining undo insert-record work per statement;
- `2048.000` remaining undo-report successes per statement;
- `0.000` remaining default-checked bulk starts per statement.

Reduced one-statement guard:

```text
MYLITE_PERF_OPEN_CLOSE_ITERATIONS=1
MYLITE_PERF_SELECT_ITERATIONS=1
MYLITE_PERF_INSERT_ITERATIONS=2048
MYLITE_PERF_BULK_INSERT_ROWS_PER_STATEMENT=2048
MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1
build/php-embedded-prod/packages/libmylite/mylite_embedded_performance_probe
```

The one-statement guard emitted zero remaining-statement averages, including
`remaining_row_ins_btr_lock_undo_undo_report`,
`remaining_trx_undo_report_assign_persistent`,
`remaining_trx_undo_report_mtr_commit`, and
`remaining_trx_undo_report_add_page`.

## Risks And Follow-Up

- More stats-enabled output can make logs wider, but it keeps the production
  probe self-contained and avoids adding another specialized binary.
- This slice identifies the next native undo target; it does not optimize or
  bypass row-level undo.
