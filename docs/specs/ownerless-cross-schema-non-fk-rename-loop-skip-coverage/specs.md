# Ownerless Cross-Schema Non-FK Rename-Loop Skip Coverage

## Problem

Same-schema non-FK rename-loop coverage proves MariaDB DDL-log rollback for a
three-pair `RENAME TABLE` swap after each native file operation. Cross-schema
non-FK rename lists still need the same inside-loop evidence because they move
native `.frm` and `.ibd` files between schema directories before MariaDB
advances the DDL-log phase.

This slice covers the deterministic cross-schema three-pair swap:

```sql
RENAME TABLE
  app.left TO archive.tmp,
  archive.right TO app.left,
  archive.tmp TO archive.right
```

Crashes after the first, second, or final native pair must roll back to the
original `app.left` and `archive.right` layout, keep the archive schema, and
leave the temporary target absent.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `do_rename()` calls `mysql_rename_table()` for each rename pair, including
    cross-schema pairs that move native files between schema directories.
  - MyLite records each successful native pair with
    `mylite_ownerless_dictionary_native_file_op()` and calls
    `mylite_ownerless_innodb_test_fault("rename-table-after-native-file-op")`
    before MariaDB advances beyond the table-rename DDL-log phase.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  - `mylite_ownerless_innodb_test_fault()` honors
    `MYLITE_OWNERLESS_TEST_FAULT_SKIP`, giving deterministic first-, second-,
    and final-pair crash points for the three-pair list.

## Scope And Non-Goals

In scope:

- Add a hook-only cross-schema non-FK rename-loop selector.
- Register standalone hook CTests for skip counts `0`, `1`, and `2`.
- Verify recovery restores the original schema/table placement and tablespace
  identities after each crash point.
- Verify native file-operation marker retention while a peer is live, final
  no-live marker drain, ownerless/native reopen, and forced `.shm` rebuild.

Out of scope:

- Mixed temporary/permanent rename-list loop matrices.
- Deterministic same-schema and cross-schema three-pair `IF EXISTS`
  rename-loop variants, covered by the
  `ownerless-rename-if-exists-list-loop-coverage` follow-up.
- Arbitrary longer rename lists or randomized rename permutations.
- Broader DDL/file-lifecycle recovery, native redo/checkpoint reconciliation,
  active-reader pressure oracle breadth, and external MariaDB/RQG stress.
- SQL-level local table-lock fault injection.

## Design

No production code changes are required. The test reuses the existing unsafe
`rename-table-after-native-file-op` hook and the existing cross-schema swap SQL.
The new assertion differs from the completed-prefinish cross-schema test: it
expects original `app.left` and `archive.right` placement, original InnoDB
`SPACE` ids, archive schema presence, and absence of the temporary target.

## Compatibility Impact

No SQL syntax, C API, storage format, or runtime behavior changes. The slice
adds evidence that ownerless live-peer recovery preserves MariaDB's native
DDL-log rollback semantics for cross-schema non-FK rename-loop crash points.

## Directory And Lifecycle Impact

No directory layout changes. The test proves recovered native files are back in
their original schema directories, temporary files are absent, marker retention
continues while a live peer exists, final no-live close drains the marker, and
forced `.shm` rebuild keeps the recovered state.

## Native Storage Impact

No storage-format changes. MariaDB native DDL-log recovery and InnoDB
dictionary rollback remain authoritative for the covered cross-schema crash
points.

## Build, Size, And Dependencies

No binary-size, dependency, or license impact.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with the
  `ownerless-test-hooks` preset.
- Run the direct cross-schema selector with no skip,
  `MYLITE_OWNERLESS_TEST_FAULT_SKIP=1`, and
  `MYLITE_OWNERLESS_TEST_FAULT_SKIP=2`.
- Run the registered hook CTests for same-schema, cross-schema, and adjacent FK
  rename-loop crash coverage.
- Run the production embedded build target for the same test binary and a
  production multi-rename smoke selector.
- Run production CI-build guards, format checks, and `git diff --check`.

## Acceptance Criteria

- Crashes after the first, second, and final native cross-schema rename pairs
  all recover to the original schema/table names and tablespace identities.
- The archive schema remains present and the temporary table/files are absent.
- Peer writes after live recovery persist through final no-live marker drain,
  ownerless/native reopen, and forced `.shm` rebuild.
- The CTests have separate names and timing in CI.
- Compatibility docs no longer list cross-schema non-FK later-pair rename-loop
  points as a remaining planned gap.

## Risks And Open Questions

- This closes the deterministic cross-schema non-FK three-pair swap only.
  Mixed temporary/permanent rename lists, `IF EXISTS` loop variants, longer
  rename permutations, and external randomized DDL oracles remain completion
  work.
