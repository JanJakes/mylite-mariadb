# Ownerless DML Marker Deadlock Outcome

## Problem

The ownerless DML file-operation marker must represent committed DML evidence,
not file-operation redo from a transaction that InnoDB rolled back as a
deadlock victim. The successful rollback slice covers explicit `ROLLBACK`, but
deadlock cleanup reaches a separate MyLite error path:
`rollback_active_transaction_after_deadlock()` preserves the MariaDB `1213`
diagnostic while internally rolling back the active transaction.

Before this slice, that internal rollback cleared
`ownerless_transaction_has_local_write` before the DML file-op redo discard
helper ran. A later user-visible `ROLLBACK` therefore could not prove that a
local write had occurred, leaving process-local file-op redo evidence available
for misclassification by a later successful statement.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::log_file_op()` records
  native file-operation redo and the MyLite fork notes the evidence through
  `mylite_ownerless_innodb_note_file_op_redo()`.
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::name_write()` writes
  `FILE_MODIFY` when a non-predefined persistent tablespace is modified for
  the first time since `fil_names_clear()`.
- `packages/libmylite/src/database.cc:rollback_active_transaction_after_deadlock()`
  handles MariaDB errno `1213` after direct or prepared statement execution
  fails.
- `packages/libmylite/src/database.cc:rollback_active_transaction()` resets
  ownerless explicit transaction state after a successful internal rollback.
- `packages/libmylite/src/database.cc:discard_ownerless_native_file_op_redo_after_rolled_back_write()`
  consumes process-local ownerless InnoDB file-op redo only when the transaction
  outcome is a rollback after local writes.

## Scope And Non-Goals

In scope:

- Snapshot whether the deadlock victim transaction had local ownerless writes
  before the internal rollback clears transaction state.
- Consume pending ownerless InnoDB file-op redo after the internal rollback
  succeeds.
- Preserve the original MariaDB `1213` diagnostic.
- Add focused two-process SQL coverage that proves the deadlock victim clears
  its process-local file-op redo latch, while the winning transaction can still
  commit and a later no-live ownerless close can drain normally after both
  child processes are reaped.

Out of scope:

- SQL-level table-lock fault injection.
- Crash windows during deadlock cleanup.
- Killed sessions, savepoint matrices, and broader concurrent-writer outcome
  classification.
- Parsing native redo payloads to distinguish every DML-origin `FILE_MODIFY`
  shape.
- Broader redo/checkpoint reconciliation, DDL/file lifecycle recovery, or
  external MariaDB/RQG stress.

## Design

`rollback_active_transaction_after_deadlock()` now snapshots
`db.ownerless_transaction_has_local_write` while the connection is still in the
deadlock victim transaction. It then calls the existing internal rollback
helper. If that rollback succeeds, MyLite calls
`discard_ownerless_native_file_op_redo_after_rolled_back_write()` with the
pre-rollback local-write snapshot and restores the original MariaDB error
snapshot.

The discard stays after successful rollback for the same reason as explicit
`ROLLBACK`: failed cleanup must not lose evidence prematurely. The discard also
remains process-local and does not suppress marker publication by a separate
winner process that commits the deadlock cycle. The focused test drains durable
winner-side marker evidence with a fresh ownerless open/close after the parent
has reaped both children, because each child close can still observe the other
deadlock participant as live or zombie.

## Compatibility Impact

No SQL syntax, public C API, diagnostics, native InnoDB format, or directory
layout changes. Deadlock victims still return MariaDB errno `1213`. The only
observable MyLite change is narrower durable ownerless checkpoint evidence:
file-op redo from a deadlock-rolled-back local write is no longer available for
later committed-DML marker attribution.

## Directory And Lifecycle Impact

No new files or checkpoint record formats are introduced. The existing
`concurrency/mylite-concurrency.ckpt` DML marker remains the committed-DML
checkpoint-needed record. A deadlock victim after local writes clears only its
process-local ownerless InnoDB file-op redo latch.

## Native Storage Impact

Native redo, undo, data, and tablespace formats are unchanged. InnoDB remains
responsible for deadlock detection and transaction rollback. MyLite classifies
the ownerless file-op redo evidence after InnoDB rollback has completed.

## Binary Size And Dependencies

The slice adds a small deadlock-path predicate snapshot and focused SQL test
coverage. It adds no dependencies and does not change the embedded MariaDB
profile.

## Test And Verification Plan

- Add `native-explicit-dml-deadlock-file-op-marker-discard` to
  `mylite_ownerless_cross_process_sql_test`.
- Add the deadlock outcome test to the weighted ownerless SQL case list.
- Run focused deadlock, rollback, commit, autocommit, and multi-peer DML marker
  selectors in the production embedded preset.
- Run transaction-adjacent ownerless SQL cases and the full production
  ownerless SQL CTest subset.
- Run the production-build guard, format check, and `git diff --check`.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  native-explicit-dml-deadlock-file-op-marker-discard` passed.
- Adjacent production selectors passed:
  `deadlock-rows`,
  `native-explicit-dml-rollback-file-op-marker-discard`,
  `native-single-owner-explicit-dml-file-op-marker-drain`,
  `native-multi-peer-explicit-dml-file-op-marker-drain`, and
  `native-dml-file-op-marker-drain`.
- Weighted transaction-adjacent production cases passed:
  `sql-case test_ownerless_explicit_dml_deadlock_discards_file_op_marker`,
  `sql-case test_two_processes_deadlock_on_innodb_rows`, and
  `sql-case test_ownerless_explicit_dml_rollback_discards_file_op_marker`.
- Full production ownerless SQL shard coverage passed 16/16 under
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.' --parallel 2
  --output-on-failure` in 223.16s real time.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  native-file-modify-redo-observation` passed.
- `tools/check-ci-production-builds` and `ctest --preset prod -R
  '^tools\.ci-production-builds$' --output-on-failure` passed.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.

## Acceptance Criteria

- A deadlock victim after checkpointed explicit-transaction DML returns the
  MariaDB deadlock diagnostic.
- The deadlock victim's process-local ownerless InnoDB file-op redo flag is
  consumed after MyLite's internal rollback.
- The winning transaction remains able to commit.
- A later no-live ownerless close after both deadlock children are reaped leaves
  both native file-op markers clear and the ownerless WAL checkpointed.
- Ownerless reopen after forced `.shm` rebuild and ordinary native reopen see
  one committed transaction's row changes.

## Risks

- This covers the deterministic row-deadlock DML shape used by existing
  ownerless lock coverage, not every possible native deadlock topology.
- The file-op redo flag remains type-agnostic; the test forces the same
  checkpointed file-per-table evidence path as the commit and rollback marker
  slices.
- Crash, killed-session, savepoint, and randomized external oracle coverage
  remain planned.
