# Ownerless Rename IF EXISTS List Loop Coverage

## Problem

Ownerless crash coverage already proves single-pair `RENAME TABLE IF EXISTS`
rollback at MariaDB's native rename-loop fault point. Same-schema and
cross-schema non-FK multi-pair `RENAME TABLE` swaps also prove first-, second-,
and final-pair rollback. The remaining syntax gap is the same multi-pair
native-loop path with the `IF EXISTS` option enabled.

This slice covers deterministic same-schema and cross-schema three-pair
`RENAME TABLE IF EXISTS` swaps. Crashes after any successful native rename pair
must roll back through MariaDB's DDL log to the original table names, original
schema placement, and original InnoDB `SPACE` identities.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_rename.cc`
  - `mysql_rename_tables()` accepts `IF EXISTS` and passes the option into the
    common `rename_tables()` / `do_rename()` path.
  - `do_rename()` calls `mysql_rename_table()` for each base-table pair.
  - After each successful native pair, MyLite records the native file operation
    with `mylite_ownerless_dictionary_native_file_op()` and calls
    `mylite_ownerless_innodb_test_fault("rename-table-after-native-file-op")`
    before MariaDB advances the DDL-log phase.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  - `mylite_ownerless_innodb_test_fault()` honors
    `MYLITE_OWNERLESS_TEST_FAULT_SKIP`, so hook tests can kill after the
    first, second, or final matching rename-loop callback.

## Scope And Non-Goals

In scope:

- Add hook-only selectors for same-schema and cross-schema multi-pair
  `RENAME TABLE IF EXISTS` native-loop crashes.
- Register standalone CTests for skip counts `0`, `1`, and `2` for each
  selector.
- Reuse the existing rollback oracles for original table names, schema
  placement, row contents, InnoDB `SPACE` identities, temporary-name absence,
  live-peer marker retention, no-live marker drain, ownerless/native reopen,
  and forced `.shm` rebuild.

Out of scope:

- Missing-source `IF EXISTS` no-op paths that do not reach native file
  operations.
- Mixed temporary/permanent rename-list loop matrices.
- Arbitrary longer rename lists or randomized rename permutations.
- Foreign-key `IF EXISTS` rename-list permutations, covered by the
  `ownerless-fk-rename-if-exists-list-loop-coverage` follow-up.
- Broader DDL/file-lifecycle recovery, native redo/checkpoint reconciliation,
  active-reader pressure oracle breadth, and external MariaDB/RQG stress.
- SQL-level local table-lock fault injection.

## Design

No production code changes are required. The test harness keeps the existing
multi-pair rollback tests and selects the `IF EXISTS` SQL spelling through a
hook-test environment flag set by the new direct selectors. This lets the new
CTest names reuse the same database-state oracle while proving that MariaDB's
`IF EXISTS` parser path reaches the same native file-operation hook for
existing base-table rename pairs.

## Compatibility Impact

No SQL syntax, C API, storage format, or runtime behavior changes. The slice
adds evidence that ownerless live-peer recovery preserves MariaDB DDL-log
rollback semantics for the covered multi-pair `RENAME TABLE IF EXISTS`
native-loop crash points.

## Directory And Lifecycle Impact

No directory layout changes. The tests prove recovered `.frm` and `.ibd` files
match the original table names and schema directories, temporary target files
are absent, native file-operation markers remain while a peer is live, final
no-live close drains the marker, and forced `.shm` rebuild keeps the recovered
state.

## Native Storage Impact

No storage-format changes. MariaDB native DDL-log recovery and InnoDB
dictionary rollback remain authoritative for the covered crash points.

## Build, Size, And Dependencies

No binary-size, dependency, or license impact.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with the
  `ownerless-test-hooks` preset.
- Run the direct same-schema and cross-schema selectors with no skip,
  `MYLITE_OWNERLESS_TEST_FAULT_SKIP=1`, and
  `MYLITE_OWNERLESS_TEST_FAULT_SKIP=2`.
- Run the registered hook CTest subset for ordinary and `IF EXISTS`
  same-schema/cross-schema rename-loop crash coverage.
- Build the production embedded target for the same test binary and run a
  production multi-rename smoke selector.
- Run production CI-build guards, format checks, and `git diff --check`.

## Acceptance Criteria

- Crashes after the first, second, and final native rename pairs for both
  same-schema and cross-schema `IF EXISTS` swaps recover to original table names
  and original InnoDB `SPACE` identities.
- The temporary table and native files are absent after recovery.
- The cross-schema archive schema remains present.
- Peer writes after live recovery persist through final no-live marker drain,
  ownerless/native reopen, and forced `.shm` rebuild.
- The CTests have separate names and timing in CI.
- Compatibility docs no longer list deterministic same-schema or cross-schema
  multi-pair `IF EXISTS` rename-loop coverage as planned.

## Risks And Open Questions

- This closes only deterministic existing-table three-pair non-FK
  `IF EXISTS` swaps. Missing-source no-op paths, mixed temporary/permanent
  lists, arbitrary longer lists, and randomized DDL oracles remain completion
  work; focused FK `IF EXISTS` lists are covered by the
  `ownerless-fk-rename-if-exists-list-loop-coverage` follow-up.
