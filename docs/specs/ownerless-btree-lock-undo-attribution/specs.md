# Ownerless B-Tree Lock/Undo Attribution

## Problem

The `c965e610` production CI run split clustered optimistic B-tree insertion
and showed that the ownerless-minus-ordinary optimistic B-tree delta is
concentrated in `btr_cur_ins_lock_and_undo()`. The stats-enabled embedded CI
sample reported:

- clustered-low ownerless-minus-ordinary total: `0.219 ms/insert`,
- row-level clustered-low MTR commit: `0.115 ms/insert`,
- clustered optimistic B-tree total: `0.100 ms/insert`,
- `btr_cur_ins_lock_and_undo()`: `0.099 ms/insert`,
- tuple insertion: `0.001 ms/insert`, and
- no optimistic B-tree fallback or error counts.

Before optimizing lock or undo behavior, we need to know whether the
`btr_cur_ins_lock_and_undo()` cost is lock checking, undo record reporting,
system-field writes, primary-leaf gating, or rare error exits.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/btr/btr0cur.cc` implements
  `btr_cur_ins_lock_and_undo()` as an inline helper called by
  `btr_cur_optimistic_insert()` and `btr_cur_pessimistic_insert()`.
- The successful clustered primary-leaf path:
  - returns immediately for the combined no-undo/keep-system-field fast path,
  - resolves the current record and index from the B-tree cursor,
  - checks predicate locks for spatial indexes or record insert locks for
    ordinary indexes unless `BTR_NO_LOCKING_FLAG` is set,
  - returns successfully before undo work for non-primary or non-leaf inserts,
  - calls `trx_undo_report_row_operation()` unless `BTR_NO_UNDO_LOG_FLAG` is
    set,
  - writes the transaction id into the tuple when undo reporting returns a
    real roll pointer, and
  - writes the roll pointer system field unless `BTR_KEEP_SYS_FLAG` is set.
- The embedded autocommit insert probe uses a simple primary-key InnoDB table,
  so its nonzero optimistic lock/undo delta is clustered primary-leaf evidence.
- The function can also return lock/predicate wait errors or undo reporting
  errors before the caller attempts tuple insertion.

## Design

Add stats-gated deep InnoDB subphase counters inside
`btr_cur_ins_lock_and_undo()` for:

- fast-path skip returns,
- cursor/index setup,
- lock check total,
- predicate-lock check,
- record insert-lock check,
- non-primary/non-leaf skip returns,
- undo report,
- `DB_TRX_ID` system-field write,
- `DB_ROLL_PTR` system-field write,
- successful primary-leaf exits,
- successful non-primary/non-leaf exits, and
- error result classes.

Emit raw `mylite_perf_*_innodb_deep_row_ins_btr_lock_undo_*` values and compact
ordinary, ownerless, and ownerless-minus-ordinary per-insert summaries when
`MYLITE_PERF_OWNERLESS_PAGE_PUBLISH_STATS=1`.

Keep the existing clustered optimistic B-tree `lock_undo` aggregate unchanged.

## Affected Subsystems

- Upstream-derived InnoDB B-tree insert helper:
  `mariadb/storage/innobase/btr/btr0cur.cc`.
- MyLite deep performance counter enum:
  `mariadb/storage/innobase/include/mylite_ownerless_innodb_deep_perf.h`.
- Embedded production performance probe:
  `packages/libmylite/tests/embedded_performance_probe.c`.
- Ownerless performance specs and compatibility notes.

## Compatibility Impact

No SQL, C API, PHP API, mysqli, locking, metadata, native storage format, or
directory-lifecycle behavior changes. The slice adds opt-in performance
instrumentation only.

## Database Directory And Lifecycle Impact

No durable directory-layout changes. The performance probe continues to create
and remove temporary MyLite directories.

## Public API Impact

No public API changes.

## Native Storage Impact

No native storage behavior changes. The new counters observe the existing
MariaDB/InnoDB insert lock and undo path without changing lock acquisition,
undo logging, redo, page modification, rollback, or purge semantics.

## Build And Performance Impact

The new timers use the existing deep-counter stats gate. Stats-off production
probe output must remain unchanged and avoid clock reads for the new
subphases. Stats-enabled attribution runs add timestamp reads around
subphases and are diagnostic only.

## Test And Verification Plan

- Rebuild the production `MinSizeRel` MariaDB embedded archive after editing
  `mariadb/storage/innobase/btr/btr0cur.cc`.
- Build `mylite_embedded_performance_probe` under `php-embedded-prod`.
- Run a reduced stats-enabled production attribution probe and confirm the new
  lock/undo summary keys are present.
- Run a reduced stats-off production probe and confirm the new lock/undo
  attribution keys are absent.
- Run focused ownerless primitive/history tests plus bounded hook/stress
  confidence selectors.
- Run `tools/check-ci-production-builds`,
  `ctest --preset prod -R '^tools\.ci-production-builds$' --output-on-failure`,
  production format check, and `git diff --check`.

## Local Verification Evidence

Local production verification after rebuilding the `MinSizeRel` MariaDB
embedded archive emitted the new stats-enabled B-tree lock/undo summary keys.
The reduced 50-row sample reported:

- ownerless-minus-ordinary clustered optimistic lock/undo total:
  `0.158 ms/insert`,
- `trx_undo_report_row_operation()`: `0.156 ms/insert`,
- record insert-lock checking: `0.002 ms/insert`,
- setup, predicate-lock checking, `DB_TRX_ID` write, and `DB_ROLL_PTR` write:
  `0.000 ms/insert`,
- primary-leaf successes: `1.000` per insert for both ordinary and ownerless,
  and
- fast skips, non-primary/non-leaf skips, `DB_FAIL`, lock wait, and
  other-error counts: `0.000` per insert.

The separate stats-off 500-row production probe emitted no
`clustered_btree_lock_undo_*` or `row_ins_btr_lock_undo_*` attribution keys and
reported ownerless autocommit at `998.27 ops/s` versus ordinary autocommit at
`2091.17 ops/s`, a `0.4774` ratio.

## Acceptance Criteria

- The deep-counter enum stays aligned between MariaDB and the C performance
  probe mirror.
- Stats-enabled production attribution reports lock check, undo report,
  system-field write, skip, success, and error-result summaries for ordinary
  and ownerless autocommit inserts plus deltas.
- Stats-off production throughput output remains unchanged.
- The implementation preserves upstream `btr_cur_ins_lock_and_undo()` control
  flow and return values.
- Docs continue to avoid claiming ownerless concurrency completion.

## Risks And Unresolved Questions

- This slice identifies the dominant subphase; it does not optimize it yet.
- If undo report dominates, the next optimization must line up with the
  existing ownerless undo reuse and history-proof invariants.
- If record insert-lock checking dominates, the next optimization belongs near
  lock-table path attribution or active-reader pressure policy.
- Row-level MTR commit remains a separate nonzero performance target.
