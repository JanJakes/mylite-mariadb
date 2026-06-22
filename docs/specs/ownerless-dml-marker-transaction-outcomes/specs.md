# Ownerless DML Marker Transaction Outcomes

## Problem

The ownerless DML file-operation marker is durable evidence that a successful
ownerless DML commit observed native InnoDB file-operation redo and must reach
a native checkpoint boundary before MyLite can treat that evidence as drained.
The explicit transaction marker path originally used
`ownerless_transaction_end_has_local_write()`, which includes both `COMMIT` and
`ROLLBACK`. That made a successful rollback marker-eligible even though no SQL
row change committed.

Because the InnoDB file-op redo flag is process-global and type-agnostic,
simply skipping marker publication on rollback is not enough. Any file-op redo
evidence left by rolled-back DML must be consumed after the successful rollback
so it cannot be misattributed to a later successful statement.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::log_file_op()` records
  native file-operation redo and the MyLite fork notes the evidence through
  `mylite_ownerless_innodb_note_file_op_redo()`.
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::name_write()` writes
  `FILE_MODIFY` when a non-predefined persistent tablespace is modified for
  the first time since `fil_names_clear()`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:mtr_t::commit()` calls
  `name_write()` for dirty mini-transactions.
- `packages/libmylite/src/database.cc:sql_ends_explicit_transaction()` treats
  `COMMIT` and transaction-ending `ROLLBACK` as explicit transaction ends.
- `packages/libmylite/src/database.cc` computes transaction-end local-write
  state before resetting ownerless transaction state, which is still required
  for lock release, current-read refresh, and page-log reclaim decisions.

## Scope And Non-Goals

In scope:

- Split explicit transaction local-write outcome classification into
  commit-specific and rollback-specific predicates.
- Publish the durable DML file-op checkpoint marker only for successful
  explicit transaction commits with local writes.
- After a successful rollback of a local-write explicit transaction, consume
  and discard pending ownerless InnoDB file-op redo evidence instead of writing
  a committed-DML marker.
- Add focused SQL coverage for a checkpointed explicit transaction `UPDATE`
  that rolls back, leaves both native file-op markers clear, clears the
  process-global file-op redo flag, and preserves the pre-transaction row after
  forced `.shm` rebuild plus ordinary native reopen.

Out of scope:

- Rollback crash windows, killed sessions, deadlock victim cleanup, savepoint
  matrices, or concurrent-writer transaction outcome coverage.
- Parsing native redo payloads to distinguish DML-origin `FILE_MODIFY` from
  other file-operation classes.
- Immediate checkpointing, group commit changes, or broader redo/checkpoint
  reconciliation.
- SQL-level table-lock fault injection and external MariaDB/RQG stress.

## Design

The direct and prepared ownerless success paths still compute
`transaction_end_had_local_write` before transaction state is updated. That
value remains the correct broad signal for statement locks, current-read
refresh, transaction cleanup, and reclaim scheduling.

Marker publication now uses a narrower
`ownerless_transaction_commit_has_local_write()` predicate. It is true only
when the current successful SQL statement is `COMMIT`, the connection is still
inside an explicit transaction at pre-reset time, and that transaction recorded
local writes.

Rollback uses `ownerless_transaction_rollback_has_local_write()`. When a
successful transaction-ending rollback had local writes, MyLite consumes the
ownerless InnoDB file-op redo flag without publishing either durable marker.
That discard is deliberately after successful SQL execution and ownerless
transaction-end cleanup, so failed statements and active transactions do not
lose evidence prematurely.

Dictionary DDL remains on the existing native file-op marker path. Autocommit
non-DDL writes and successful explicit transaction commits continue to publish
the DML-specific marker when the InnoDB file-op redo flag is set.

## Compatibility Impact

No SQL syntax, public C API, diagnostics, native InnoDB format, or directory
layout changes. The change narrows MyLite's durable ownerless checkpoint
metadata: rolled-back explicit transaction DML no longer leaves committed-DML
checkpoint-needed evidence. SQL rollback semantics stay MariaDB-compatible.

## Directory And Lifecycle Impact

No new files or checkpoint record formats are introduced. The existing
`concurrency/mylite-concurrency.ckpt` DML marker remains the committed-DML
checkpoint-needed record. A successful rollback with local writes leaves that
record clear and clears only the process-local InnoDB file-op redo latch.

## Native Storage Impact

Native redo, undo, data, and tablespace formats are unchanged. InnoDB remains
responsible for rolling back SQL-visible row changes. MyLite only changes how
its ownerless lifecycle classifies the type-agnostic file-op redo evidence at
the SQL transaction boundary.

## Binary Size And Dependencies

The slice adds small helper predicates, a discard helper, and one focused SQL
test. It adds no dependencies and does not change the embedded MariaDB profile.

## Test And Verification Plan

- Add `native-explicit-dml-rollback-file-op-marker-discard` to
  `mylite_ownerless_cross_process_sql_test`.
- Add the rollback outcome test to the weighted ownerless SQL case list.
- Run focused rollback, commit, autocommit, and multi-peer DML marker
  selectors in the production embedded preset.
- Run transaction-adjacent ownerless SQL cases, ownerless hook file-modify
  observation, ownerless SQL CTest subset, production-build guard, format
  check, and `git diff --check`.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  native-explicit-dml-rollback-file-op-marker-discard` passed.
- Adjacent production selector
  `native-single-owner-explicit-dml-file-op-marker-drain` passed after the
  formatted-source rebuild; the autocommit, single-owner explicit, and
  multi-peer explicit DML marker selectors also passed before formatting.
- Weighted transaction-adjacent production cases passed:
  `sql-case test_ownerless_explicit_dml_rollback_discards_file_op_marker`,
  `sql-case test_ownerless_native_file_op_marker_drains_after_single_owner_explicit_transaction_dml`,
  `sql-case test_ownerless_explicit_transaction_undo_wal_elision`, and
  `sql-case test_ownerless_concurrent_transaction_commits`.
- Full production ownerless SQL shard coverage passed 16/16 under
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.' --parallel 2
  --output-on-failure` in 198.96s real time after formatting.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  native-file-modify-redo-observation` passed.
- `tools/check-ci-production-builds` and `ctest --preset prod -R
  '^tools\.ci-production-builds$' --output-on-failure` passed.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.

## Acceptance Criteria

- A checkpointed explicit transaction `UPDATE` does not publish any native
  file-op marker before rollback.
- Successful `ROLLBACK` keeps both the dictionary/native file-op marker and
  the DML marker clear.
- Successful `ROLLBACK` consumes any pending ownerless InnoDB file-op redo flag
  so later statements cannot inherit stale evidence.
- The pre-transaction row remains visible after forced `.shm` rebuild and after
  ordinary native read/write reopen.
- Successful explicit transaction `COMMIT` marker behavior remains covered by
  the existing focused tests.

## Risks

- This is a successful rollback outcome slice, not crash recovery for rollback
  windows.
- The file-op redo flag remains type-agnostic; the test uses the same
  checkpointed file-per-table `UPDATE` shape as the commit marker tests for
  source-backed `FILE_MODIFY` evidence.
- Deadlock, killed transaction, savepoint, and concurrent-writer matrices
  remain planned.
