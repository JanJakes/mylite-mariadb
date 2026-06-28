# Ownerless Non-FK Rename-Loop Skip Coverage

## Problem

Ownerless same-schema `RENAME TABLE` crash coverage proves the prefinish
boundary after MariaDB completes the native rename list and before MyLite
finishes the ownerless dictionary generation. It does not separately prove
crashes inside MariaDB's native rename loop after each successful native
file-operation pair.

This slice covers the deterministic non-FK three-pair swap:

```sql
RENAME TABLE
  app.left TO app.tmp,
  app.right TO app.left,
  app.tmp TO app.right
```

The required outcome for crashes after any native pair is MariaDB DDL-log
rollback to the original `left` and `right` tables, not completion of the
remaining rename pairs.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `do_rename()` calls `mysql_rename_table()` for each rename pair.
  - After a successful native pair, MyLite records the native dictionary file
    operation through `mylite_ownerless_dictionary_native_file_op()` and then
    calls `mylite_ownerless_innodb_test_fault("rename-table-after-native-file-op")`
    before MariaDB advances the DDL-log phase to trigger/statistics handling.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  - `mylite_ownerless_innodb_test_fault()` honors
    `MYLITE_OWNERLESS_TEST_FAULT_SKIP`, so hook tests can kill after the first,
    second, or final matching rename-loop fault.

## Scope And Non-Goals

In scope:

- Add a hook-only same-schema non-FK rename-loop selector.
- Register standalone CTests for skip counts `0`, `1`, and `2`.
- Verify live-peer recovery rolls the swap back to the original table names and
  tablespace identities.
- Verify the native file-operation marker remains while a peer is live, drains
  after final no-live recovery, and the recovered state survives ownerless
  reopen, ordinary native reopen, and forced `.shm` rebuild.

Out of scope:

- Cross-schema non-FK later-pair rename-loop coverage, covered by the
  `ownerless-cross-schema-non-fk-rename-loop-skip-coverage` follow-up.
- Mixed temporary/permanent rename-list loop matrices.
- `IF EXISTS` rename-loop variants beyond the existing focused coverage.
- Broader DDL/file-lifecycle recovery, native redo/checkpoint reconciliation,
  active-reader pressure oracle breadth, and external MariaDB/RQG stress.
- SQL-level local table-lock fault injection.

## Design

No production code changes are required. The test reuses the existing unsafe
`rename-table-after-native-file-op` hook and the existing three-pair same-schema
swap SQL. The recovery oracle differs from the completed-prefinish crash test:
it asserts original `left` and `right` tablespaces, original row placement, and
absence of the temporary table after MariaDB DDL-log rollback.

## Compatibility Impact

No SQL syntax, C API, storage format, or runtime behavior changes. The slice
adds compatibility evidence that MyLite's ownerless live-peer recovery preserves
MariaDB's native DDL-log rollback semantics for the covered non-FK rename loop.

## Directory And Lifecycle Impact

No directory layout changes. The test proves the recovered `.frm` and `.ibd`
files match the original same-schema table names, the temporary rename target
files are absent, ownerless marker retention does not drain while a live peer
exists, final no-live close drains the marker, and a forced `.shm` rebuild keeps
the recovered state.

## Native Storage Impact

No storage-format changes. MariaDB native DDL-log recovery and InnoDB
dictionary rollback remain authoritative for the covered crash points.

## Build, Size, And Dependencies

No binary-size, dependency, or license impact.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with the
  `ownerless-test-hooks` preset.
- Run the direct selector with no skip, `MYLITE_OWNERLESS_TEST_FAULT_SKIP=1`,
  and `MYLITE_OWNERLESS_TEST_FAULT_SKIP=2`.
- Run the registered hook CTests for same-schema non-FK and adjacent FK
  rename-loop crash coverage.
- Run the production embedded build target for the same test binary to ensure
  non-hook builds still compile.
- Run production CI-build guards, format checks, and `git diff --check`.

## Acceptance Criteria

- Crashes after the first, second, and final native rename pairs all recover to
  the original non-FK table names and tablespace identities.
- The temporary table and temporary native files are absent after recovery.
- Peer writes after live recovery persist through final no-live marker drain,
  ownerless/native reopen, and forced `.shm` rebuild.
- The CTests have separate names and timing in CI.
- Compatibility docs no longer list same-schema non-FK later-pair rename-loop
  points as a remaining planned gap.

## Risks And Open Questions

- This closes only the same-schema non-FK three-pair swap. The
  `ownerless-cross-schema-non-fk-rename-loop-skip-coverage` follow-up covers
  the deterministic cross-schema three-pair swap; broader mixed rename-list
  matrices remain completion work.
