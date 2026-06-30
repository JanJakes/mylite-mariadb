# Ownerless Row-Undo Durable Progress

## Problem Statement

Ownerless rollback crash coverage exposed a gap at the first
`rollback-after-native-row-undo` hook hit for generated columns and FK/trigger
side effects. The row change itself had been undone in memory, but a later
ownerless opener could observe stable partial native state with a header-sized
ownerless WAL and no active recovered transaction. That points to native
rollback progress durability rather than ownerless page-version replay.

Ownerless page-write hooks can make rollback-dirtied native pages durable for
cross-process visibility. Once an undone page image can reach disk, the native
undo-log progress proving that row undo must also be durable before a killed
process can be treated as a supported crash boundary.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/row/row0undo.cc:262-376` selects the latest undo
  record and advances the transaction undo cursor in memory.
- `mariadb/storage/innobase/row/row0undo.cc:386-415` applies one undo record
  through `row_undo_ins()` or `row_undo_mod()` and fires the MyLite
  `rollback-after-native-row-undo` test hook after success.
- `mariadb/storage/innobase/trx/trx0undo.cc:876-895` exposes
  `trx_undo_try_truncate()` for truncating rollback progress from the native
  undo tail.
- `mariadb/storage/innobase/log/log0log.cc:1245-1248` exposes
  `log_buffer_flush_to_disk()` for making current redo durable.

## Design

For ownerless mode only, `row_undo()` now turns each successful native row-undo
record into a durable restart point before the unsafe test hook can pause:

1. Apply the native row undo.
2. Truncate the native undo tail with `trx_undo_try_truncate(node->trx)`.
3. Flush current redo with `log_buffer_flush_to_disk()`.
4. Fire `rollback-after-native-row-undo` if unsafe hooks are enabled.

The guard is `mylite_ownerless_innodb_lock_has_hooks()`, so ordinary embedded
InnoDB rollback keeps MariaDB's original progress cadence. The cost is limited
to ownerless rollback, not normal commit paths.

## Compatibility Impact

No SQL syntax, C API, storage format, or directory-layout change. The behavior
change is stronger crash durability for ownerless native rollback progress.
Large ownerless rollbacks can pay additional redo flush cost per row-undo
record; this is a correctness tradeoff for process-kill recovery boundaries.

## Scope And Non-Goals

In scope:

- Full rollback and savepoint rollback at the shared post-`row_undo()` hook
  boundary.
- Generated-column stored/virtual values and generated-column secondary
  indexes.
- FK `ON UPDATE CASCADE`, FK `ON DELETE CASCADE`, FK `ON DELETE SET NULL`, and
  update/delete trigger audit rows.
- No-live recovery and live-peer busy gating before final no-live recovery.

Out of scope:

- Faults inside individual `row_undo_ins()` or `row_undo_mod()` substeps before
  the row-undo operation returns success.
- XA/prepared rollback, DDL rollback, broader FK action/trigger matrices,
  randomized fault selection, and external MariaDB/RQG stress.

## Test And Verification Plan

- Rebuild the embedded MariaDB archive:
  `tools/mariadb-embedded-build build`.
- Build the hook target:
  `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j$(nproc)`.
- Run focused side-effect row-undo selectors:
  `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-transaction-rollback-(generated-row-undo|generated-row-undo-live-peer|fk-trigger-row-undo|fk-trigger-row-undo-live-peer|fk-trigger-delete-row-undo|fk-trigger-delete-row-undo-live-peer)-crash$' --output-on-failure`.
- Run the adjacent rollback crash subset.
- Run production embedded smoke selectors.
- Run `tools/check-ci-production-builds`, format checks, `git diff --check`,
  and cleanup checks.

## Acceptance Criteria

- Generated-column, FK/trigger update-side, and FK/trigger delete-side
  selectors pass at the first `rollback-after-native-row-undo` hit, without
  skipping earlier row-undo hook hits.
- Native DML and generic file-operation markers remain clear.
- No-live recovery waits for recovered native transactions to drain and restores
  the original pre-transaction rows.
- Live-peer variants return `MYLITE_BUSY` while a peer remains live, then
  recover after peer release.
- Forced `.shm` rebuild, ordinary native reopen, and follow-up native writes
  observe the recovered original state.

## Verification Results

- `tools/mariadb-embedded-build build` passed and refreshed the embedded
  MariaDB archive after the `row0undo.cc` change.
- `cmake --build --preset ownerless-test-hooks --target mylite_ownerless_cross_process_sql_test -j8`
  passed.
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-transaction-rollback-(generated-row-undo|generated-row-undo-live-peer|fk-trigger-row-undo|fk-trigger-row-undo-live-peer|fk-trigger-delete-row-undo|fk-trigger-delete-row-undo-live-peer)-crash$' --output-on-failure`
  passed 6/6 with no row-undo fault skips.
- `ctest --preset ownerless-test-hooks -R '^libmylite\.ownerless-(savepoint-rollback-(before-state|prewrite-before-state|prewrite-live-peer|native-row-undo|native-row-undo-live-peer)-crash|transaction-rollback-(before-state|native-row-undo|native-row-undo-live-peer|generated-row-undo|generated-row-undo-live-peer|fk-trigger-row-undo|fk-trigger-row-undo-live-peer|fk-trigger-delete-row-undo|fk-trigger-delete-row-undo-live-peer)-crash)$' --output-on-failure`
  passed 14/14.
- `cmake --build --preset embedded-prod --target mylite_ownerless_cross_process_sql_test -j8`
  passed.
- `ctest --preset embedded-prod -R '^libmylite\.ownerless-(generated-column-index-ddl|generated-column-indexed-expression|concurrent-savepoint-rollback-handoff|random-transaction-rollback-handoff)$' --output-on-failure`
  passed 2/2.
- `ctest --preset embedded-prod -R '^(libmylite\.ownerless-explicit-transaction-(update|delete|replace|mixed-dml|order-limit)-history-proof|tools\.ownerless-(random-tx-seed-suite|transaction-stress-trace))$' --output-on-failure`
  passed 7/7.
- `tools/check-ci-production-builds`, `cmake --build --preset format-check-prod`,
  and `git diff --check` passed.

## Risks And Follow-Up

- The durability fix adds redo flush work to ownerless rollback. Add a
  rollback-specific performance probe before optimizing this path, especially
  for PHP/PHPUnit-style suites that use transaction rollback for isolation.
- This does not cover crashes inside `row_undo_ins()` or `row_undo_mod()`
  before the row-undo operation returns success.
