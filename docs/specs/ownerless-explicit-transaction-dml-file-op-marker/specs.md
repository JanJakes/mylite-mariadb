# Ownerless Explicit Transaction DML File-Op Marker

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

Current marker semantics are extended by
`docs/specs/ownerless-dml-marker-proof-split/specs.md`: explicit transaction
DML now writes a DML-specific checkpoint-needed marker that can be published
while an idle ownerless peer is live, without relaxing no-live user-page proof.

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
  proved the original single-owner boundary. The later DML-marker proof split
  removes this restriction by separating DML checkpoint evidence from the
  proof-relaxing dictionary DDL marker.

## Scope And Non-Goals

In scope:

- At successful explicit transaction `COMMIT` with local writes, consume the
  existing InnoDB file-op redo flag.
- When the flag is set, persist durable checkpoint-needed evidence in
  `concurrency/mylite-concurrency.ckpt`; the current implementation uses the
  DML-specific marker from `ownerless-dml-marker-proof-split`.
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
- Proving deadlock, killed-transaction, or crash windows for every explicit
  transaction outcome. Successful rollback marker discard is covered by
  `docs/specs/ownerless-dml-marker-transaction-outcomes/specs.md`.
- Exhaustive multi-writer explicit transaction DML-origin coverage. The
  DML-marker proof split covers an idle-peer explicit DML marker drain, and the
  transaction-outcome follow-up covers successful rollback discard, but
  deadlock, crash, and concurrent-writer matrices remain planned.
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
- the current statement is a successful explicit transaction `COMMIT` that had
  local writes.

Dictionary DDL keeps its existing pre-finish and post-DDL marker paths. The
explicit transaction commit path remains type-agnostic: if the InnoDB file-op
redo flag is set at `COMMIT`, the original slice persisted the same
checkpoint-needed marker already used for dictionary DDL and autocommit DML.
The current proof split persists the DML-specific marker instead, so explicit
DML commits can publish checkpoint-needed evidence while keeping no-live
user-page proof required. If the marker cannot be written because the runtime
or checkpoint file is unavailable, the helper re-notes the flag so a later
ownerless cleanup path can still observe it.

## Compatibility Impact

No SQL syntax, public C API, native storage format, or directory layout changes.
Successful ownerless explicit transaction commits that emit native
file-operation redo may now leave a conservative DML checkpoint-needed marker
until the existing no-live close path drains it. Successful rollback after
local writes does not publish the committed-DML marker and is covered by the
transaction-outcome follow-up. SQL results, commit/rollback semantics, and
MariaDB diagnostics are unchanged.

## Directory And Lifecycle Impact

The original slice wrote only the existing native file-op marker records in
`concurrency/mylite-concurrency.ckpt`. The current proof split appends
DML-specific marker records in the same checkpoint file. No new durable files
are introduced. The marker remains rebuild-safe because `.shm` is volatile
coordination state and durable checkpoint-needed truth lives in the database
directory.

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
- Add `native-multi-peer-explicit-dml-file-op-marker-drain` in the DML-marker
  proof split.
- Add `native-explicit-dml-rollback-file-op-marker-discard` in the
  transaction-outcome follow-up.
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
- The successful `COMMIT` sets durable DML file-op checkpoint-needed evidence.
- The successful rollback follow-up proves rolled-back explicit DML leaves that
  committed-DML marker clear and clears stale process-local file-op evidence.
- The savepoint follow-up proves DML rolled back by `ROLLBACK TO SAVEPOINT`
  before any earlier local write survives does not publish committed-DML marker
  evidence at the later `COMMIT`.
- Final no-live ownerless close clears the marker only after native checkpoint
  proof succeeds.
- Committed data remains readable after forced `.shm` rebuild and ordinary
  native reopen.
- Autocommit DML marker behavior remains covered.
- Docs describe this as bounded explicit transaction commit marker coverage,
  with broader DML-origin, crash, killed-transaction, killed-session savepoint, and
  concurrent-writer matrices still planned.

## Risks

- The marker is type-agnostic, so the `FILE_MODIFY` conclusion depends on the
  source-backed checkpointed file-per-table DML setup.
- Deadlock and savepoint rollback outcomes are covered by follow-up slices;
  killed explicit-transaction and crash windows remain unproven by this slice.
- Multi-peer explicit DML marker coverage is bounded to the idle-peer proof
  split case; concurrent-writer and crash outcomes remain unproven.
- Broader native redo/checkpoint reconciliation, DDL/file-lifecycle recovery,
  and external MariaDB/RQG stress remain planned.
