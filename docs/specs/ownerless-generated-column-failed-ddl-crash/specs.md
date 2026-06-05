# Ownerless Generated Column Failed DDL Crash

## Problem Statement

Ownerless generated-column policy coverage proves MariaDB-compatible rejection
for representative invalid generated-column definitions, including blocked
functions and generated-column primary keys. Those tests cover ordinary failed
DDL completion, but they do not prove recovery when a writer dies after MariaDB
rejects the DDL and before MyLite publishes ownerless dictionary finish.

MyLite must clean up that active ownerless dictionary state without inventing
native metadata for rejected tables or columns, and without changing the
MariaDB errno that a later retry reports.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/field.cc:10814-10827` validates generated-column expressions
  through `Item::check_vcol_func_processor`, rejects impossible functions, and
  rejects non-strictly-deterministic functions for stored generated columns.
- `mariadb/sql/field.h:564-573` defines the generated-column validation flag
  classes, including `VCOL_IMPOSSIBLE`, `VCOL_NON_DETERMINISTIC`,
  `VCOL_SESSION_FUNC`, `VCOL_TIME_FUNC`, and
  `VCOL_NOT_STRICTLY_DETERMINISTIC`.
- `mariadb/sql/sql_table.cc:2958-2963` rejects a `PRIMARY KEY` whose key part
  targets a generated column and returns
  `ER_PRIMARY_KEY_BASED_ON_GENERATED_COLUMN`.
- `mariadb/libmariadb/include/mysqld_error.h` assigns errno `1901` to
  `ER_GENERATED_COLUMN_FUNCTION_IS_NOT_ALLOWED` and errno `1903` to
  `ER_PRIMARY_KEY_BASED_ON_GENERATED_COLUMN`.
- `packages/libmylite/src/database.cc` begins ownerless dictionary DDL before
  executing ownerless dictionary statements. Direct and prepared execution both
  call `ownerless_finish_dictionary_ddl()` after MariaDB statement errors, so
  the unsafe `dictionary-before-finish` hook can kill a writer after MariaDB
  has rejected generated-column DDL but before MyLite marks the dictionary
  state idle.

## Design

Add a hook-only selector,
`dictionary-generated-column-failed-crash`, to
`mylite_ownerless_cross_process_sql_test`.

The selector:

1. Creates a valid InnoDB table with a deterministic stored generated column.
2. Kills a writer at `dictionary-before-finish` after MariaDB rejects a
   create-time stored `RAND()` generated column with errno `1901`.
3. Verifies a live peer keeps cleanup busy until no-live ownerless recovery.
4. Reopens ownerless and verifies no rejected `.frm` or `.ibd` files and no
   `INFORMATION_SCHEMA.TABLES` row exist for the rejected table.
5. Retries the same rejected create and verifies MariaDB still returns errno
   `1901`.
6. Repeats the crash for a failed `ALTER TABLE ... ADD COLUMN` stored
   `RAND()` generated column, then proves the existing table keeps its original
   generated column, rows, and values.
7. Repeats the crash for a generated-column primary-key `CREATE TABLE`, then
   proves the rejected table remains absent and a retry still returns errno
   `1903`.
8. Verifies the final valid table through ownerless and ordinary native reopen,
   before and after forced `.shm` rebuild, including a small insert/delete
   round trip.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for representative failed
  generated-column `CREATE TABLE`,
- crash-at-dictionary-before-finish coverage for representative failed
  generated-column `ALTER TABLE`,
- crash-at-dictionary-before-finish coverage for generated-column primary-key
  rejection,
- no leaked rejected table files, columns, indexes, or table metadata,
- stable retry errno `1901` and `1903`,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- exhaustive replay of MariaDB's blocked generated-column function matrix,
- successful generated-column DDL crash injection beyond the representative
  create/alter/index coverage in
  `docs/specs/ownerless-generated-column-success-ddl-crash/specs.md`,
- generated-column foreign-key crash injection beyond the representative ADD
  CONSTRAINT coverage in
  `docs/specs/ownerless-generated-column-foreign-key-crash/specs.md`,
- SQL-level table-lock fault injection for native table-wait paths,
- external MariaDB/RQG long-running generated-column oracle stress.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless generated
column compatibility evidence by proving rejected generated-column DDL remains
MariaDB-compatible after a writer dies while MyLite's ownerless dictionary
state is still active.

This is not a claim of complete generated-column crash recovery. It covers
representative failed generated-column validation paths; representative
successful generated-column create/alter/index crash coverage is tracked by
`docs/specs/ownerless-generated-column-success-ddl-crash/specs.md`.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The selector verifies that rejected
generated-column create statements do not leave native metadata files under
`datadir/app/`, specifically:

- `ownerless_generated_failed_crash_invalid_create.frm`,
- `ownerless_generated_failed_crash_invalid_create.ibd`,
- `ownerless_generated_failed_crash_generated_pk.frm`,
- `ownerless_generated_failed_crash_generated_pk.ibd`.

The surviving valid table remains an ordinary native InnoDB table inside the
MyLite database directory.

## Native Storage Impact

No InnoDB file format or redo behavior changes. Failed generated-column DDL
does not reach successful native table metadata publication. The slice only
proves ownerless dictionary recovery after those MariaDB validation failures.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with `ownerless-test-hooks`.
- Run the focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-generated-column-failed-crash`
- Run adjacent hook crash selectors around constraints, generated columns, and
  columns.
- Run adjacent embedded generated-column policy selectors.
- Run ownerless SQL CTest filters, ownerless stress, `ctest --preset dev`,
  `format-check`, `tidy`, and `git diff --check`.

## Acceptance Criteria

- Each failed DDL writer reaches `dictionary-before-finish` and can be killed
  without hanging the test.
- A live ownerless peer prevents cleanup until no-live recovery.
- Rejected create-time generated-column tables have no native files or
  `INFORMATION_SCHEMA.TABLES` rows after recovery.
- Failed generated-column ALTER leaves the existing table definition, rows, and
  deterministic generated values intact.
- Retrying the rejected generated-function DDL returns MariaDB errno `1901`.
- Retrying the generated-column primary-key DDL returns MariaDB errno `1903`.
- Ownerless and ordinary native reopen observe the same final state before and
  after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic failed-DDL crash coverage, not exhaustive
  generated-column crash fuzzing.
- Successful generated-column DDL crash coverage beyond the representative
  create/alter/index and generated-column FK ADD CONSTRAINT selectors remains
  planned.
- Full external MariaDB/RQG long-running generated-column stress remains
  environment-owned follow-up work.
