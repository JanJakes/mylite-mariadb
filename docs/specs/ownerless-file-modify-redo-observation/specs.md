# Ownerless File-Modify Redo Observation

## Problem Statement

Ownerless file-operation marker tests prove durable checkpoint-needed evidence
for several DDL file lifecycles, but true InnoDB `FILE_MODIFY` evidence is
harder to target from SQL. MariaDB emits `FILE_MODIFY` when a persistent
non-predefined tablespace is modified after checkpoint bookkeeping has reset
that tablespace, not from a distinct SQL statement type.

This slice originally added a bounded hook-build proof that ordinary DML on a
file-per-table InnoDB table can reach that `FILE_MODIFY` redo path. A later
`ownerless-dml-file-op-marker` slice promotes the checkpointed-DML case to
bounded durable marker coverage for autocommit writes by consuming the same
flag during successful write cleanup, so the hook selector now observes the
durable marker rather than taking the raw flag after SQL returns. This
observation slice by itself does not claim durable marker coverage for every
DML `FILE_MODIFY` case.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::name_write()` starts around
  line `3177` and writes `FILE_MODIFY` when a non-predefined persistent
  tablespace is modified for the first time since `fil_names_clear()`.
- `mariadb/storage/innobase/fil/fil0fil.cc:fil_names_clear()` starts around
  line `3196`, clears `space->max_lsn` for tablespaces dirtied before the
  checkpoint LSN, and writes checkpoint-side `FILE_MODIFY` records.
- `mariadb/storage/innobase/mtr/mtr0mtr.cc:mtr_t::commit()` checks
  `m_user_space && !m_user_space->max_lsn` around line `4774` and calls
  `name_write()` before finishing the mini-transaction.
- `mariadb/storage/innobase/buf/buf0flu.cc` documents around line `2077` why
  checkpoint code repeats `FILE_MODIFY` records before `FILE_CHECKPOINT`.
- `mariadb/storage/innobase/fil/fil0fil.cc:mtr_t::log_file_op()` starts around
  line `1607` and calls `mylite_ownerless_innodb_note_file_op_redo()` for
  logged native file-operation redo, including `FILE_MODIFY`.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  exposes `mylite_ownerless_innodb_make_checkpoint()` and
  `mylite_ownerless_innodb_take_file_op_redo()` for hook-build observation.

## Scope And Non-Goals

In scope:

- Add a hook-only `native-file-modify-redo-observation` selector and CTest.
- Create and modify a file-per-table InnoDB table in ownerless mode.
- Force a native checkpoint, clear the existing ownerless file-op redo flag,
  run a DML update, and assert the production cleanup consumed the flag by
  setting the native file-op checkpoint-needed marker.
- Verify the table remains readable and writable after the observation.

Out of scope:

- Distinguishing the native file-op type in persisted MyLite marker records.
- Claiming that every DML shape publishes a durable ownerless checkpoint marker.
- Changing MariaDB redo, InnoDB checkpoint, MyLite page-version WAL, or
  concurrency checkpoint formats.
- SQL-level table-lock fault injection or external MariaDB/RQG stress.

## Design

The test opens an ownerless read/write database, creates
`app.ownerless_file_modify_redo`, inserts one row, and forces a native
checkpoint with `mylite_ownerless_innodb_make_checkpoint()`. The checkpoint may
itself write file-op redo, so the test clears any existing bit with
`mylite_ownerless_innodb_take_file_op_redo()`.

It then executes:

```sql
UPDATE app.ownerless_file_modify_redo SET value = 11 WHERE id = 1
```

No DDL file operation occurs in that statement. After the
`ownerless-dml-file-op-marker` follow-up, the production successful-write
cleanup consumes the same file-op flag and persists the native
checkpoint-needed marker. A set marker after the update is therefore
source-backed evidence that InnoDB emitted file-operation redo from the
post-checkpoint DML path, which is the `FILE_MODIFY` path described above.

## Compatibility Impact

No SQL result, C API, PHP API, native storage format, production behavior, or
directory layout changes. The slice strengthens unsafe-hook evidence for
ownerless native redo/checkpoint behavior.

## DDL Metadata Routing Impact

No DDL metadata routing changes. The SQL under observation after the
checkpoint is DML.

## Directory And Lifecycle Impact

The test uses only existing MyLite-owned directory contents:

- `datadir/app/ownerless_file_modify_redo.frm`,
- `datadir/app/ownerless_file_modify_redo.ibd`,
- ordinary InnoDB redo files inside the MyLite directory.

No new directory entries are introduced.

## Native Storage Impact

No native format changes. The test observes MariaDB-native `FILE_MODIFY`
bookkeeping through the existing MyLite ownerless InnoDB hook flag.

## Build, Size, License, And Dependencies

No dependency, license, public API, or production binary-size impact. The new
CTest is registered only when unsafe ownerless test hooks are enabled.

## Test And Verification Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run `native-file-modify-redo-observation` directly.
- Run focused CTest registration for the new selector.
- Run adjacent native file-operation marker selectors.
- Run production `native-file-op-marker-drain` to prove normal production
  behavior still builds and runs.
- Run `tools/check-ci-production-builds`, `format-check-prod`, and
  `git diff --check`.

## Verification Results

- `cmake --build --preset ownerless-test-hooks --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- Direct hook selector `native-file-modify-redo-observation` passed before the
  durable-marker follow-up; after that follow-up, the selector asserts the
  marker produced by the consumed flag instead of taking the flag directly.
- `ctest --preset ownerless-test-hooks -R
  '^libmylite\.ownerless-native-file-modify-redo-observation$'
  --output-on-failure` passed 1/1.
- Focused hook file-op group passed 10/10 for the new observation plus rename,
  create-like, CTAS, replacement-copy, force-rebuild, row-format, truncate, and
  drop marker crash selectors.
- `cmake --build --preset php-embedded-prod --target
  mylite_ownerless_cross_process_sql_test -j2` passed.
- Production selector `native-file-op-marker-drain` passed.
- `tools/check-ci-production-builds` passed with
  `ci_production_build_audit_ok=.github/workflows/ci.yml`.
- `ctest --preset prod -R '^tools\.ci-production-builds$'
  --output-on-failure` passed 1/1.
- `cmake --build --preset format-check-prod` passed.
- `git diff --check` passed.

## Acceptance Criteria

- The test can force a native checkpoint while the ownerless database is open.
- Any pre-existing file-op redo flag is cleared before the observed DML.
- A post-checkpoint `UPDATE` sets the native file-op checkpoint-needed marker
  through the production successful-write cleanup path.
- The updated row is visible before close and after reopen.
- The selector is registered as a hook-only CTest.

## Risks And Follow-Up

- The hook flag is type-agnostic. The `FILE_MODIFY` conclusion depends on the
  source-backed absence of DDL file operations in the observed statement.
- Durable marker coverage for all DML-origin `FILE_MODIFY` cases remains
  unclaimed; the later `ownerless-dml-file-op-marker` slice covers the
  focused checkpointed autocommit-DML case only.
- Broader native redo/checkpoint reconciliation and external MariaDB/RQG stress
  remain planned.
