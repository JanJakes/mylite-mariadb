# Ownerless DML Marker Proof Split

## Problem

The original ownerless DML file-operation marker reused the durable native
file-op checkpoint-needed marker that dictionary DDL uses. That marker both
forces native checkpoint drain and relaxes no-live page-LSN proof during page-log
reclaim. The relaxation is intentional for some file-lifecycle DDL boundaries,
but it is too broad for DML-origin `FILE_MODIFY`: user data/index page WAL still
must prove the native page on disk covers the retained ownerless page image
before MyLite can truncate the WAL.

This blocked broader explicit-transaction DML marker publication. A previous
single-owner slice could publish the old marker only when no peer joined the
epoch; applying the same marker to multi-peer explicit DML had already disturbed
the concurrent commit-race case.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::log_file_op()` records
  native file-operation redo and the MyLite fork notes that evidence through
  `mylite_ownerless_innodb_note_file_op_redo()`.
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::name_write()` emits
  `FILE_MODIFY` when a non-predefined persistent tablespace is modified after
  `fil_names_clear()`.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:mtr_t::commit()` calls
  `name_write()` for dirty mini-transactions.
- `packages/libmylite/src/database.cc:reclaim_ownerless_page_log_after_native_checkpoint()`
  previously treated `native_file_op_checkpoint_marker_needed` as both a
  native-checkpoint trigger and a reason to skip user-page native LSN proof.
- `packages/libmylite/src/database.cc:prepare_ownerless_page_log_native_checkpoint_for_reclaim()`
  already accepts an explicit `require_native_page_lsn_proof` flag, making the
  marker/proof split local to reclaim policy rather than requiring a page-log
  format change.

## Scope And Non-Goals

In scope:

- Add a DML-specific durable checkpoint-needed marker in
  `concurrency/mylite-concurrency.ckpt`.
- Make successful autocommit DML and successful explicit transaction `COMMIT`
  after local writes publish that DML marker when the InnoDB file-op redo flag
  is set.
- Let the DML marker force native checkpoint drain and startup uncheckpointed
  file-operation recovery.
- Keep no-live user-page LSN/payload proof required when only the DML marker is
  present.
- Preserve the existing native file-op marker and legacy fallback for
  dictionary DDL/file-lifecycle boundaries.
- Add focused SQL coverage for a multi-peer explicit transaction DML commit
  with an idle ownerless peer present.

Out of scope:

- Parsing native redo payloads or classifying every `FILE_MODIFY` source.
- Changing dictionary DDL/file-lifecycle proof relaxation.
- Deadlock, killed-transaction, or crash coverage for every explicit
  transaction outcome. Successful rollback marker discard is covered by
  `docs/specs/ownerless-dml-marker-transaction-outcomes/specs.md`.
- Immediate checkpointing at every DML commit, background scheduling, group
  commit, or broader redo/checkpoint reconciliation.
- External MariaDB/RQG stress and SQL-level table-lock fault injection.

## Design

`mylite-concurrency.ckpt` now appends two checksummed generation records after
the existing native file-op marker records. The new record uses magic
`MYLCDML1`, the same fixed record size and generation/needed/checksum fields as
the existing marker, and no legacy scalar fallback. Reads prefer the highest
valid generation record; non-empty invalid DML-marker records fail closed as
checkpoint-needed evidence.

The existing native file-op marker remains unchanged:

- dictionary DDL and dictionary-finish paths still write it;
- legacy marker fallback remains available for older checkpoint files;
- no-live reclaim may still relax page-LSN proof when that marker or the
  AUTO_INCREMENT checkpoint-pending bit is present.

The DML marker is separate:

- autocommit non-DDL write cleanup and explicit transaction `COMMIT` cleanup
  consume `mylite_ownerless_innodb_take_file_op_redo()`;
- when the flag is set, they write the DML marker under the runtime/checkpoint
  lock;
- if the marker cannot be written, they re-note the flag for later cleanup;
- no-live reclaim includes the DML marker in the native-checkpoint trigger set
  but does not include it in the proof-relaxing marker set.

This lets multi-peer explicit DML publish durable checkpoint-needed evidence
without letting ordinary user page-version WAL truncate unless the native page
proof succeeds.

## Compatibility Impact

No SQL syntax, diagnostics, public C API, or native InnoDB file format changes.
The durable MyLite checkpoint file grows by two appended DML-marker slots. Older
fields and the existing native file-op marker offsets remain stable.

Successful ownerless DML that emits native file-operation redo may now leave a
DML-specific checkpoint-needed marker until final no-live cleanup drains it.
SQL commit visibility and MariaDB/InnoDB locking behavior are unchanged.

## Directory And Lifecycle Impact

The only directory-layout change is an appended marker record pair inside
`concurrency/mylite-concurrency.ckpt`. `.shm` remains volatile rebuildable
state. The DML marker is durable database-directory evidence and is cleared
only after native checkpoint/reclaim preparation succeeds.

Startup treats either native file-op marker as uncheckpointed file-operation
recovery evidence, so ordinary reopen after retained DML marker state installs
the same InnoDB recovery hooks used for existing file-operation boundaries.

## Native Storage Impact

Native InnoDB redo, data, undo, and tablespace formats are unchanged. The slice
observes MariaDB's existing file-operation redo evidence and changes only
MyLite's directory-owned checkpoint metadata and reclaim policy.

## Binary Size And Dependencies

The implementation adds no dependencies and keeps the MariaDB embedded profile
unchanged. The code change is limited to MyLite-owned checkpoint marker helpers,
ownerless reclaim policy, ownerless startup marker reads, and focused SQL
tests.

## Test Plan

- Add `native-multi-peer-explicit-dml-file-op-marker-drain` to
  `mylite_ownerless_cross_process_sql_test`.
- Update autocommit and single-owner explicit DML marker coverage to assert the
  DML marker while the legacy native file-op marker remains clear.
- Keep dictionary DDL marker coverage on the existing native file-op marker.
- Run focused DML/DDL marker selectors, commit-race, explicit undo elision,
  hook-only native file-modify observation, ownerless SQL shards, ownerless
  stress, production-build guard, format check, and whitespace checks.

## Verification Results

- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/php-embedded-prod/packages/libmylite/mylite_ownerless_cross_process_sql_test
  native-multi-peer-explicit-dml-file-op-marker-drain` passed.
- Production adjacent selectors `native-dml-file-op-marker-drain`,
  `native-single-owner-explicit-dml-file-op-marker-drain`, and
  `native-file-op-marker-drain` passed.
- Transaction-adjacent production SQL cases passed:
  `sql-case test_ownerless_concurrent_transaction_commits`,
  `sql-case test_ownerless_explicit_transaction_undo_wal_elision`, and
  `sql-case test_ownerless_native_dml_marker_drains_after_multi_peer_explicit_transaction_dml`.
- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test
  native-file-modify-redo-observation` passed.
- `ctest --preset php-embedded-prod -R
  '^libmylite\.ownerless-cross-process-sql\.' --parallel 2
  --output-on-failure` passed 16/16 in 215.57s real time.
- `cmake --preset ownerless-stress` passed.
- `cmake --build --preset ownerless-stress --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- `ctest --preset ownerless-stress --output-on-failure` passed 12/12 in
  425.50s real time.
- `tools/check-ci-production-builds` passed.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- Autocommit and explicit-transaction DML-origin file-op redo writes the
  DML-specific checkpoint-needed marker, not the proof-relaxing DDL marker.
- A live idle ownerless peer can be present while explicit transaction DML
  commits and publishes the DML marker.
- Successful explicit transaction rollback after local writes leaves the DML
  marker clear and discards stale process-local file-op evidence.
- The DML marker remains set while that peer keeps the runtime live, then clears
  after final no-live native checkpoint/reclaim.
- User page-version WAL reclaim still requires native page LSN/payload proof
  when only the DML marker is present.
- The old dictionary DDL marker behavior and legacy marker fallback remain
  covered.
- Commit-race coverage remains stable.

## Risks

- The DML marker is still type-agnostic; the focused tests rely on
  source-backed checkpointed file-per-table `UPDATE` shapes to produce
  `FILE_MODIFY`.
- Killed-transaction, deadlock, savepoint, and crash windows remain planned
  follow-up matrices.
- If DDL and DML markers are both present, the DDL marker still permits the
  existing proof relaxation. That preserves current file-lifecycle behavior but
  leaves mixed DDL/DML proof interaction for broader native redo/checkpoint
  reconciliation.
- External randomized DML/RQG stress remains planned.
