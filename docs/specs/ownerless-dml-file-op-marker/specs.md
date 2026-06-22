# Ownerless DML File-Op Marker

## Problem

Ownerless native file-operation coverage proved many dictionary DDL paths that
persist the checkpoint-needed marker in
`concurrency/mylite-concurrency.ckpt`. A later hook-only slice proved that
ordinary DML on a file-per-table InnoDB table can emit native `FILE_MODIFY`
redo after a checkpoint, but production ownerless statement cleanup did not
consume that evidence outside dictionary DDL. That left the DML-origin
`FILE_MODIFY` class as observation evidence rather than durable marker
coverage.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::log_file_op()` records all
  native file-operation redo, including `FILE_MODIFY`, and the MyLite fork
  calls `mylite_ownerless_innodb_note_file_op_redo()` before writing the redo
  payload.
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::name_write()` writes
  `FILE_MODIFY` when a non-predefined persistent tablespace is modified for
  the first time since `fil_names_clear()`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:mtr_t::commit()` calls
  `name_write()` when a mini-transaction dirties such a tablespace with
  `max_lsn == 0`.
- `packages/libmylite/src/database.cc` already persists and drains the native
  file-op checkpoint-needed marker for dictionary DDL, and final no-live close
  clears the marker only after `mylite_ownerless_innodb_make_checkpoint()`
  succeeds.

## Scope And Non-Goals

In scope:

- After a successful autocommit non-DDL ownerless write statement, consume the
  existing InnoDB file-op redo flag.
- When the flag is set, persist the existing native file-op checkpoint-needed
  marker in `mylite-concurrency.ckpt`.
- Restore the flag if marker persistence is unavailable, matching the
  dictionary-finish fallback.
- Add focused SQL coverage for a checkpointed ownerless autocommit `UPDATE`
  that emits DML-origin file-op redo, marks the checkpoint-needed bit, drains
  it on final no-live close, and preserves data across forced `.shm` rebuild
  plus native reopen.

Out of scope:

- Parsing native redo payload types or distinguishing `FILE_MODIFY` from other
  file-operation redo in the marker.
- Immediate native checkpointing after every DML-origin `FILE_MODIFY`.
- Single-owner explicit-transaction DML-origin marker coverage, which is
  handled separately by
  `docs/specs/ownerless-explicit-transaction-dml-file-op-marker/specs.md`;
  multi-peer explicit DML remains separate follow-up work.
- New checkpoint scheduling policy, background checkpoint workers, or broad
  redo/checkpoint reconciliation.
- SQL-level table-lock fault injection or external MariaDB/RQG stress.

## Design

Direct and prepared ownerless statement success paths now call a narrow
post-write helper after dictionary DDL cleanup. The helper returns unless the
handle is ownerless read/write, the statement began outside an explicit
transaction, the statement is a successful non-DDL write, and
`mylite_ownerless_innodb_take_file_op_redo()` reports native file-operation
redo. When those conditions hold, it writes the existing native file-op
checkpoint-needed marker under the runtime mutex. If the runtime or checkpoint
file is unavailable, the helper re-notes the flag so a later ownerless
statement path can still observe the evidence.

Dictionary DDL keeps its existing pre-finish and post-DDL marker/checkpoint
paths. This slice deliberately does not add immediate checkpoint work to the
ordinary DML hot path; final no-live close and the existing reclaim paths own
the checkpoint proof.

## Compatibility Impact

No SQL syntax, public C API, native storage format, or directory layout changes.
Successful ownerless autocommit DML that emits native file-operation redo may
now leave a conservative checkpoint-needed marker until the existing final
no-live close drains it. SQL results and MariaDB diagnostics are unchanged.

## Directory And Lifecycle Impact

The slice writes only the existing native file-op marker records in
`concurrency/mylite-concurrency.ckpt`. No new files are introduced. The marker
still drains through the existing native checkpoint path and is rebuild-safe
because `.shm` remains volatile coordination state, not durable truth.

## Native Storage Impact

Native InnoDB redo and tablespace formats are unchanged. The new production
path observes MariaDB's existing `FILE_MODIFY` evidence through the MyLite
file-op redo flag and records that a later native checkpoint is required
before MyLite can treat the file-operation boundary as fully drained.

## Binary Size And Dependencies

The implementation adds one small first-party helper and a focused SQL test.
It adds no dependencies and does not change the embedded MariaDB profile.

## Test Plan

- Add `native-dml-file-op-marker-drain` to
  `mylite_ownerless_cross_process_sql_test`.
- Add the focused test to the weighted ownerless SQL case list.
- Run the focused selector in the production embedded preset.
- Run the adjacent native marker/reclaim selectors and hook-only
  `native-file-modify-redo-observation`.
- Run ownerless SQL shard coverage, ownerless stress, production-build guard,
  format check, and diff whitespace checks.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  native-dml-file-op-marker-drain` passed.
- Production adjacent selectors `native-file-op-marker-drain`, `native-reclaim`,
  and `sql-case test_ownerless_native_file_op_marker_drains_after_checkpointed_dml`
  passed.
- The initial broader helper disturbed
  `test_ownerless_concurrent_transaction_commits`; after narrowing the helper
  to autocommit statements, `sql-case test_ownerless_concurrent_transaction_commits`
  passed.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- Hook selector `native-file-modify-redo-observation` passed after updating it
  to assert the marker produced by the consumed flag.
- Hook file-op crash subset passed 10/10 for native file-modify observation
  plus dictionary rename, create-like, CTAS, replacement-copy, force-rebuild,
  row-format, truncate, and drop marker crash selectors.
- Full production ownerless SQL shard coverage passed 16/16 under
  `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.' --parallel 2 --output-on-failure`.
- `cmake --preset ownerless-stress`, `cmake --build --preset ownerless-stress
  --target mylite_ownerless_cross_process_sql_test -j2`, and
  `ctest --preset ownerless-stress --output-on-failure` passed 12/12.
- `tools/check-ci-production-builds` and `ctest --preset prod -R
  '^tools\.ci-production-builds$' --output-on-failure` passed.
- `cmake --build --preset format-check-prod` and `git diff --check` passed.

## Acceptance Criteria

- A checkpointed ownerless autocommit `UPDATE` on a file-per-table InnoDB table
  sets the native file-op checkpoint-needed marker before handle close.
- Final no-live ownerless close clears the marker only after native checkpoint
  proof succeeds.
- Data remains readable after forced `.shm` rebuild and ordinary native reopen.
- Dictionary DDL marker behavior remains unchanged.
- The compatibility matrix records this as bounded DML-origin marker coverage,
  with broader redo/checkpoint reconciliation still planned.

## Risks

- The marker is type-agnostic; the `FILE_MODIFY` conclusion depends on the
  source-backed checkpointed DML setup.
- This slice does not prove every possible DML shape that can emit
  `FILE_MODIFY`; single-owner explicit-transaction DML commit coverage is
  handled by
  `docs/specs/ownerless-explicit-transaction-dml-file-op-marker/specs.md`,
  while multi-peer explicit DML remains follow-up work.
- Broader native redo/checkpoint reconciliation, DDL/file-lifecycle recovery,
  and external MariaDB/RQG stress remain planned.
