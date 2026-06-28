# Ownerless Rename IF EXISTS Loop Coverage

## Problem

Ownerless `RENAME TABLE IF EXISTS` coverage currently proves the completed
prefinish boundary after the source has moved to the target and before MyLite
finishes ownerless dictionary publication. It does not prove the earlier native
rename-loop crash point after MariaDB has completed the native file move but
before the DDL-log phase advances.

This slice covers an existing-table `RENAME TABLE IF EXISTS` crash at that
native-loop point. Recovery must follow MariaDB DDL-log rollback, preserving
the original source table and leaving the target absent.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `mysql_rename_tables()` accepts optional `IF EXISTS` syntax and executes
    the same native `do_rename()` loop for existing base-table pairs.
  - `do_rename()` records each successful native file operation with
    `mylite_ownerless_dictionary_native_file_op()` and calls
    `mylite_ownerless_innodb_test_fault("rename-table-after-native-file-op")`
    before MariaDB advances beyond the table-rename DDL-log phase.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - `dictionary-rename-if-exists-crash` covers the completed
    `dictionary-before-finish` boundary and expects target-table recovery.

## Scope And Non-Goals

In scope:

- Add a hook-only `RENAME TABLE IF EXISTS` native-loop crash selector.
- Verify live-peer recovery rolls back to the original source table, original
  InnoDB `SPACE` id, and absent target.
- Verify native file-operation marker retention while a peer is live, final
  no-live marker drain, ownerless/native reopen, and forced `.shm` rebuild.
- Register a standalone hook CTest with visible timing.

Out of scope:

- Missing-source `RENAME TABLE IF EXISTS` no-op paths that do not reach the
  native file-operation hook.
- Longer `IF EXISTS` rename-list permutations.
- Mixed temporary/permanent rename-list loop matrices.
- Broader DDL/file-lifecycle recovery, native redo/checkpoint reconciliation,
  active-reader pressure oracle breadth, and external MariaDB/RQG stress.

## Design

No production code changes are required. The test reuses the existing unsafe
`rename-table-after-native-file-op` hook with the existing `RENAME TABLE IF
EXISTS app.source TO app.target` SQL. The oracle records the original source
tablespace id, crashes after the native rename pair, opens a live ownerless
peer, and verifies MariaDB DDL-log rollback restored the source table.

## Compatibility Impact

No SQL syntax, C API, storage format, or runtime behavior changes. The slice
adds evidence that ownerless live-peer recovery preserves MariaDB DDL-log
rollback for the covered `RENAME TABLE IF EXISTS` native-loop crash point.

## Directory And Lifecycle Impact

No directory layout changes. The test proves recovered native files are present
under the source name, target files are absent, marker retention continues while
a live peer exists, final no-live close drains the marker, and forced `.shm`
rebuild keeps the recovered state.

## Native Storage Impact

No storage-format changes. MariaDB native DDL-log recovery and InnoDB
dictionary rollback remain authoritative for the covered IF EXISTS crash point.

## Build, Size, And Dependencies

No binary-size, dependency, or license impact.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with the
  `ownerless-test-hooks` preset.
- Run the direct `dictionary-rename-if-exists-loop-crash` selector.
- Run the adjacent registered rename CTests.
- Build the production embedded target for the same test binary and run a
  production rename smoke selector.
- Run production CI-build guards, format checks, and `git diff --check`.

## Acceptance Criteria

- The native-loop crash recovers to the original source table and original
  tablespace id, with the target absent.
- Peer writes after live recovery persist through final no-live marker drain,
  ownerless/native reopen, and forced `.shm` rebuild.
- The CTest has a separate name and timing in CI.
- Compatibility docs describe focused IF EXISTS native-loop coverage and keep
  longer IF EXISTS rename-list permutations as planned.

## Risks And Open Questions

- This is a single existing-table IF EXISTS rename pair. Longer IF EXISTS
  rename-list permutations remain completion work.
