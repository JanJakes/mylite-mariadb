# Ownerless FK Rename IF EXISTS List Loop Coverage

## Problem

Ownerless foreign-key multi-rename native-loop coverage already proves
MariaDB DDL-log rollback for same-schema and cross-schema parent/child rename
lists. The remaining syntax gap is the same existing-table FK rename-list path
with `IF EXISTS` enabled.

This slice proves that `RENAME TABLE IF EXISTS` over existing FK parent/child
tables reaches the same MariaDB native rename loop and preserves FK metadata
and enforcement after deterministic native-loop crashes.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `mysql_rename_tables()` accepts `IF EXISTS` and passes the option through
    the common `rename_tables()` / `do_rename()` path.
  - `check_rename()` only turns a missing source into a skippable note when
    `IF EXISTS` applies; existing base tables continue to `do_rename()`.
  - `do_rename()` records each successful native table rename with
    `mylite_ownerless_dictionary_native_file_op()` and calls
    `mylite_ownerless_innodb_test_fault("rename-table-after-native-file-op")`
    before MariaDB advances the DDL-log phase.
- `mariadb/storage/innobase/handler/ha_innodb.cc` and
  `mariadb/storage/innobase/row/row0mysql.cc`
  - InnoDB updates dictionary table names and foreign-key references during
    native table rename using normalized `db/table` identifiers.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  - `mylite_ownerless_innodb_test_fault()` honors
    `MYLITE_OWNERLESS_TEST_FAULT_SKIP`, allowing deterministic first/later
    native rename-pair crash points.

## Scope And Non-Goals

In scope:

- Same-schema three-pair existing-table FK `RENAME TABLE IF EXISTS` native-loop
  crashes after the first, second, and final native rename pair.
- Cross-schema two-pair existing-table FK `RENAME TABLE IF EXISTS` native-loop
  crashes after the first and second native rename pair.
- Reuse the existing FK rollback oracles for original parent/child table names,
  row contents, FK metadata/enforcement, native file placement, live-peer
  marker retention, no-live marker drain, ownerless/native reopen, and forced
  `.shm` rebuild.

Out of scope:

- Missing-source `IF EXISTS` no-op paths that do not reach the native file
  operation hook.
- Mixed temporary/permanent rename-list matrices.
- Arbitrary longer rename lists or randomized rename permutations.
- Broader DDL/file-lifecycle recovery, native redo/checkpoint reconciliation,
  active-reader pressure oracle breadth, and external MariaDB/RQG stress.
- SQL-level local table-lock fault injection.

## Design

No production code changes are required. The hook test harness adds
`IF EXISTS` selector names that set `MYLITE_OWNERLESS_TEST_RENAME_IF_EXISTS=1`
before calling the existing same-schema and cross-schema FK native-loop
rollback tests. The SQL helpers choose the `RENAME TABLE IF EXISTS` spelling
when that environment flag is present; otherwise they keep the ordinary
`RENAME TABLE` spelling for the existing selectors.

The same recovery oracle remains authoritative: MariaDB DDL-log rollback must
restore the original FK parent/child names and constraints rather than
completing the remaining rename pairs.

## Compatibility Impact

No SQL syntax, C API, storage-format, or product behavior changes. The slice
adds hook-build evidence that MyLite ownerless recovery preserves MariaDB
native DDL-log rollback semantics for covered existing-table FK
`RENAME TABLE IF EXISTS` lists.

## Directory And Lifecycle Impact

No directory layout changes. The tests verify the recovered `.frm` and `.ibd`
files remain in the original schema directories, moved/intermediate files are
absent after rollback, the native file-operation marker remains while a peer is
live, final no-live close drains the marker, and forced `.shm` rebuild keeps
the recovered state.

## Native Storage Impact

No storage-format changes. MariaDB native DDL-log recovery and InnoDB
foreign-key dictionary rollback remain authoritative for the covered crash
points.

## Build, Size, And Dependencies

No binary-size, dependency, or license impact.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with the
  `ownerless-test-hooks` preset.
- Run direct same-schema and cross-schema FK `IF EXISTS` native-loop selectors
  with the registered skip counts.
- Run the registered hook CTest subset for ordinary and `IF EXISTS`
  same-schema/cross-schema FK rename-loop crash coverage.
- Build the production embedded test target and run a production FK
  multi-rename smoke selector.
- Run production CI-build guards, format checks, and `git diff --check`.

## Acceptance Criteria

- Covered same-schema FK `RENAME TABLE IF EXISTS` crashes after first, second,
  and final native rename pairs recover to the original parent/child tables.
- Covered cross-schema FK `RENAME TABLE IF EXISTS` crashes after first and
  second native rename pairs recover to the original parent/child tables and
  leave the target schema without moved tables.
- FK metadata and enforcement still reference the original parent table after
  recovery.
- Marker retention, no-live drain, ownerless/native reopen, and forced `.shm`
  rebuild behavior match the existing ordinary FK native-loop coverage.
- Compatibility docs no longer list existing-table FK `IF EXISTS` rename-list
  permutations as a remaining planned gap.

## Risks And Open Questions

- This closes only deterministic existing-table FK `IF EXISTS` rename lists.
  Missing-source no-op paths, mixed temporary/permanent lists, arbitrary longer
  lists, and randomized DDL oracles remain completion work.
