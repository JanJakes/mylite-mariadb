# Ownerless Generated-Column Foreign Key

## Problem

Ownerless foreign-key coverage now proves ordinary, composite, deep cascade,
rename, and cross-schema foreign-key behavior, and generated-column coverage
proves create-time and ALTER add/drop refresh. It still does not prove the
supported intersection: stored generated columns participating in InnoDB
foreign keys while already-open ownerless peers observe enforcement,
referential actions, generated values, and reopen durability.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/mysql-test/suite/gcol/r/gcol_keys_innodb.result` records that
  stored generated child foreign-key columns are accepted for `ON UPDATE
  RESTRICT`/`NO ACTION`, `ON DELETE RESTRICT`, `ON DELETE CASCADE`, and
  `ON DELETE NO ACTION`, while `ON UPDATE CASCADE`, `ON UPDATE SET NULL`, and
  `ON DELETE SET NULL` are rejected with
  `ER_WRONG_FK_OPTION_FOR_GENERATED_COLUMN`.
- The same upstream result shows virtual generated child foreign-key columns
  are rejected in normal `CREATE TABLE` / `ALTER TABLE` paths as incorrectly
  formed foreign keys.
- `mariadb/storage/innobase/dict/dict0crea.cc` rejects `SET NULL` or
  `CASCADE` constraints on base columns of stored columns through
  `dict_foreigns_has_s_base_col()`, preserving InnoDB's generated-column
  consistency rules.
- `mariadb/storage/innobase/handler/handler0alter.cc` has a parallel
  `innobase_check_fk_stored()` guard for ALTER-time foreign-key additions over
  base columns of stored generated columns.
- `mariadb/storage/innobase/include/dict0mem.h` tracks virtual columns
  affected by foreign keys through `dict_foreign_t::v_cols`, and
  `mariadb/storage/innobase/row/row0ins.cc` fills virtual-column values while
  executing cascaded actions when such dependent columns are present.

## Scope And Non-Goals

In scope:

- Cross-process ownerless SQL coverage for a stored generated child column used
  as the foreign-key column with `ON UPDATE RESTRICT ON DELETE CASCADE`.
- Cross-process ownerless SQL coverage for a stored generated referenced column
  with a unique index and a regular child foreign key using
  `ON UPDATE RESTRICT ON DELETE CASCADE`.
- Already-open ownerless peer visibility for generated values, constraint
  metadata, missing-parent errors, restricted parent-key updates, cascaded
  deletes, and valid inserts after the cascade boundary.
- Final ownerless and native exclusive reopen checks before and after forced
  `.shm` rebuild.

Out of scope:

- Virtual generated child foreign-key columns.
- Generated-column foreign keys with MariaDB-rejected `ON UPDATE CASCADE`,
  `ON UPDATE SET NULL`, or `ON DELETE SET NULL` clauses.
- Foreign keys on base columns of stored generated columns with MariaDB-rejected
  action clauses.
- Generated-column FK DDL crash/error injection.
- Partitioned-table or special-index generated-column foreign keys.

## Design

- Add a focused `generated-column-foreign-key` selector to
  `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates two table pairs:
  - `generated_child`: a child table whose stored generated `parent_key`
    references a regular parent primary key.
  - `generated_ref`: a parent table whose stored generated `parent_key` is
    unique and referenced by a regular child column.
- The parent keeps an already-open ownerless peer and verifies generated values
  plus `REFERENTIAL_CONSTRAINTS` metadata for both constraints.
- The child attempts parent-key updates that must fail with MariaDB errno 1451
  under `ON UPDATE RESTRICT`; the peer verifies generated values remain stable.
- The peer verifies missing-parent inserts fail with MariaDB errno 1452 for
  both generated-column FK shapes.
- The child deletes parent rows that must cascade child rows; the peer verifies
  the cascaded deletes, inserts new valid rows through both FK shapes, and
  commits.
- Final state is verified through ownerless read/write and ordinary native
  exclusive reopen before and after deleting
  `concurrency/mylite-concurrency.shm`.

## Compatibility Impact

This adds bounded ownerless evidence for MariaDB-supported stored
generated-column foreign-key shapes. It does not claim unsupported virtual
generated child FKs, MariaDB-rejected generated-column action clauses, or
generated-column FK crash/error recovery.

## Directory And Lifecycle Impact

No new files or layout changes. The slice uses native InnoDB files inside the
MyLite database directory and validates final state after volatile
shared-memory rebuild.

## Native Storage Impact

No storage-format changes. The test relies on MariaDB/InnoDB native generated
column evaluation, generated-column index metadata, foreign-key enforcement,
cascaded delete execution, and MyLite's existing ownerless page visibility,
redo visibility, and reopen paths.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `generated-column-foreign-key` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer observes both generated-column FK constraints
  and generated column values created by another ownerless process.
- Parent-key updates rejected by `ON UPDATE RESTRICT` fail with MariaDB errno
  1451 and leave generated FK rows unchanged.
- Missing-parent inserts fail with MariaDB errno 1452 for both generated-column
  FK shapes.
- Parent deletes cascade through both generated-column FK shapes.
- Valid rows can be inserted through both FK shapes after the cascade boundary.
- The final rows and constraint metadata survive ownerless/native reopen before
  and after forced `.shm` rebuild.

## Risks And Follow-Up

- This proves two supported stored generated-column FK shapes. Virtual
  generated child FKs, MariaDB-rejected generated-column action clauses,
  external randomized FK graph stress, and crash/error injection during FK
  execution remain follow-up compatibility or recovery work.
