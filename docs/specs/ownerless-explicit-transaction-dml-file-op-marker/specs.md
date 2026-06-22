# Ownerless Single-Owner Explicit Transaction DML File-Op Marker

## Problem

The ownerless DML file-operation marker slice persists the native
checkpoint-needed marker after successful autocommit DML observes InnoDB
file-operation redo. Explicit transactions remained outside that helper: DML
inside the transaction could set MyLite's native file-op redo flag, but the
statement cleanup path intentionally skipped marker publication while the
transaction was active and did not consume the flag at `COMMIT`.

That left a bounded correctness gap after a native checkpoint for the simplest
explicit-transaction topology: a continuous single-owner ownerless handle could
modify a file-per-table InnoDB table, emit `FILE_MODIFY` redo, commit
successfully, and close without persisting the checkpoint-needed marker that
tells no-live recovery to publish the matching native checkpoint boundary
before treating the file-operation evidence as drained.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::log_file_op()` records
  native file-operation redo. The MyLite fork notes this through the
  ownerless InnoDB file-op redo flag before the payload is written.
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::name_write()` writes
  `FILE_MODIFY` when a non-predefined persistent tablespace is modified for
  the first time since `fil_names_clear()`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:mtr_t::commit()` calls
  `name_write()` as dirty mini-transactions are committed.
- `packages/libmylite/src/database.cc` already detects
  `ownerless_transaction_end_has_local_write()` before transaction state is
  reset, and uses that fact to refresh current reads at transaction end.
- `packages/libmylite/src/database.cc` previously skipped
  `mark_ownerless_native_file_op_checkpoint_after_successful_write()` for any
  statement that started inside an explicit transaction, including `COMMIT`.
- `packages/libmylite/src/database.cc:ownerless_runtime_in_single_owner_epoch_locked()`
  proves the current process is the only active ownerless process and that the
  process-registry generation still matches the owner generation, so no peer
  joined after this handle registered.

## Scope And Non-Goals

In scope:

- At successful explicit transaction end with local writes during a continuous
  single-owner epoch, consume the existing InnoDB file-op redo flag.
- When the flag is set, persist the existing native file-op checkpoint-needed
  marker in `concurrency/mylite-concurrency.ckpt`.
- Preserve the existing fallback that re-notes the flag if marker persistence
  is unavailable.
- Add focused SQL coverage for a checkpointed ownerless explicit transaction
  that updates a file-per-table InnoDB table, observes no marker before
  `COMMIT`, observes the marker after `COMMIT`, drains it on final no-live
  close, and verifies the committed row after forced `.shm` rebuild plus
  native reopen.

Out of scope:

- Parsing native redo payloads or classifying every DML-origin `FILE_MODIFY`
  shape.
- Proving rollback, deadlock, killed-transaction, or crash windows for every
  explicit transaction outcome.
- Multi-peer explicit transaction DML-origin marker coverage. The existing
  native checkpoint marker also relaxes no-live page-LSN proof during reclaim;
  that remains unsafe to apply to multi-writer explicit DML without a broader
  marker/proof split.
- Immediate checkpointing at `COMMIT`, background checkpoint scheduling,
  group commit, or broader redo/checkpoint reconciliation.
- SQL-level table-lock fault injection and external MariaDB/RQG stress.

## Design

The direct and prepared ownerless success paths compute
`transaction_end_had_local_write` before calling
`update_ownerless_transaction_state_after_successful_sql()`, matching the
existing current-read refresh decision. The native file-op marker helper now
accepts that pre-reset fact.

The helper publishes the marker when either of these conditions is true:

- the current statement is a successful autocommit non-DDL write, preserving
  the previous DML marker behavior;
- the current statement successfully ends an explicit transaction that had
  local writes, and the process registry still proves a continuous
  single-owner epoch.

Dictionary DDL keeps its existing pre-finish and post-DDL marker paths. The
explicit transaction path remains type-agnostic: if the InnoDB file-op redo
flag is set at a single-owner transaction end, MyLite persists the same
checkpoint-needed marker already used for dictionary DDL and autocommit DML. If
the marker cannot be written because the runtime or checkpoint file is
unavailable, the helper re-notes the flag so a later ownerless cleanup path can
still observe it. Multi-peer explicit transactions leave the flag unconsumed in
this slice rather than persisting a marker whose reclaim semantics are not yet
proven for multi-writer DML page-version WAL.

## Compatibility Impact

No SQL syntax, public C API, native storage format, or directory layout changes.
Successful ownerless explicit transaction commits in a continuous single-owner
epoch that emit native file-operation redo may now leave a conservative
checkpoint-needed marker until the existing no-live close path drains it. SQL
results, commit semantics, and MariaDB diagnostics are unchanged.

## Directory And Lifecycle Impact

The slice writes only the existing native file-op marker records in
`concurrency/mylite-concurrency.ckpt`. No new durable files are introduced.
The marker remains rebuild-safe because `.shm` is volatile coordination state
and durable checkpoint-needed truth lives in the database directory.

## Native Storage Impact

Native InnoDB redo, page, undo, and tablespace formats are unchanged. The
implementation observes MariaDB's existing file-operation redo evidence through
the MyLite ownerless flag and records that a later native checkpoint is needed
before the ownerless lifecycle can consider the file-operation boundary fully
drained.

## Binary Size And Dependencies

The implementation extends an existing helper signature and adds one focused
SQL test. It adds no dependencies and does not change the embedded MariaDB
profile.

## Test Plan

- Add `native-single-owner-explicit-dml-file-op-marker-drain` to
  `mylite_ownerless_cross_process_sql_test`.
- Add the focused test to the weighted ownerless SQL case list.
- Run the focused selector and adjacent autocommit DML marker selector in the
  production embedded preset.
- Run transaction-adjacent ownerless SQL cases and the hook-only native
  file-modify observation selector.
- Run ownerless SQL shard coverage, production-build guard, format check, and
  diff whitespace checks before commit.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  native-single-owner-explicit-dml-file-op-marker-drain` passed.
- Adjacent production selectors/cases passed:
  `native-dml-file-op-marker-drain`, the new `sql-case` entry,
  `sql-case test_ownerless_concurrent_transaction_commits`, and
  `sql-case test_ownerless_explicit_transaction_undo_wal_elision`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  native-file-modify-redo-observation` passed.
- Full production ownerless SQL shard coverage passed 16/16 under
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.' --parallel 2
  --output-on-failure`.
- `cmake --preset ownerless-stress` and `cmake --build --preset
  ownerless-stress --target mylite_ownerless_cross_process_sql_test -j2`
  passed.
- The first `ctest --preset ownerless-stress --output-on-failure` run hit an
  intermittent DDL-stress `ownerless dictionary statement lock is busy`
  assertion. The isolated DDL-stress rerun passed, and the full ownerless
  stress rerun then passed 12/12.
- `tools/check-ci-production-builds` and `ctest --preset prod -R
  '^tools\.ci-production-builds$' --output-on-failure` passed.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.

## Acceptance Criteria

- A checkpointed single-owner ownerless explicit transaction `UPDATE` on a
  file-per-table InnoDB table does not set the checkpoint-needed marker before
  `COMMIT`.
- The successful `COMMIT` sets the native file-op checkpoint-needed marker.
- Final no-live ownerless close clears the marker only after native checkpoint
  proof succeeds.
- Committed data remains readable after forced `.shm` rebuild and ordinary
  native reopen.
- Autocommit DML marker behavior remains covered.
- Docs describe this as bounded single-owner explicit transaction commit marker
  coverage, with broader DML-origin, multi-peer, and crash-recovery matrices
  still planned.

## Risks

- The marker is type-agnostic, so the `FILE_MODIFY` conclusion depends on the
  source-backed checkpointed file-per-table DML setup.
- Rollback and killed explicit-transaction windows remain unproven by this
  slice.
- Multi-peer explicit DML marker coverage remains unproven because the current
  marker also changes reclaim proof policy.
- Broader native redo/checkpoint reconciliation, DDL/file-lifecycle recovery,
  and external MariaDB/RQG stress remain planned.
