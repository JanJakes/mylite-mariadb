# Ownerless Generated-Column Foreign-Key Policy

## Problem

Ownerless generated-column foreign-key coverage proves MariaDB-supported stored
generated child and referenced-column shapes. The remaining documented gap still
calls out virtual generated child foreign keys and MariaDB-rejected
generated-column action clauses. Source inspection and focused probing show
MariaDB rejects only specific generated-column action clauses, while virtual
generated child columns can participate in ordinary restricted/cascading foreign
keys when indexed. Ownerless read/write mode should prove both the accepted
virtual-child shape and the rejected action clauses under
`MYLITE_OPEN_OWNERLESS_RW`.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/mysql-test/suite/gcol/inc/gcol_keys.inc:134-151` expects
  `ER_WRONG_FK_OPTION_FOR_GENERATED_COLUMN` for stored generated child foreign
  keys with `ON UPDATE SET NULL`, `ON UPDATE CASCADE`, and
  `ON DELETE SET NULL`, both at `CREATE TABLE` time and at `ALTER TABLE` time.
- `mariadb/sql/sql_class.cc:309-327` rejects foreign keys whose local column is
  generated when the action is `ON DELETE SET NULL`, `ON UPDATE SET NULL`, or
  `ON UPDATE CASCADE`; `mariadb/include/mysql.h` aliases the historical virtual
  error name to the generated-column error.
- `mariadb/sql/sql_alter.cc:293-304` marks generated-column foreign-key action
  clauses as invalid when the generated column expression would be affected.
- `mariadb/storage/innobase/include/dict0mem.h:1550-1557` stores virtual
  columns affected by a foreign key, and
  `mariadb/storage/innobase/row/row0ins.cc:872-912` plus
  `row0ins.cc:1248-1282` fill virtual-column information during cascaded child
  operations.
- `mariadb/libmariadb/include/mysqld_error.h:895-896` defines
  `ER_KEY_BASED_ON_GENERATED_VIRTUAL_COLUMN` as 1904 and
  `ER_WRONG_FK_OPTION_FOR_GENERATED_COLUMN` as 1905.

## Scope And Non-Goals

In scope:

- Add ownerless SQL coverage where a child ownerless process attempts the
  MariaDB-rejected generated-column FK forms while a peer handle remains open.
- Assert `ER_WRONG_FK_OPTION_FOR_GENERATED_COLUMN` for `CREATE TABLE` and
  `ALTER TABLE` forms using a stored generated child column with:
  - `ON UPDATE SET NULL`,
  - `ON UPDATE CASCADE`,
  - `ON DELETE SET NULL`.
- Verify virtual generated child FK `CREATE TABLE` and `ALTER TABLE` with
  `ON UPDATE RESTRICT ON DELETE CASCADE` are accepted, enforce missing-parent
  and restricted-update failures, and cascade deletes.
- Verify the already-open peer sees no failed child tables or failed
  constraints, observes the accepted virtual child constraints, and can still
  write the valid policy tables.
- Verify final policy-table state through ownerless/native reopen before and
  after forced `.shm` rebuild.

Out of scope:

- Adding support for the rejected MariaDB action forms.
- External randomized foreign-key graph stress.
- Crash injection inside referential-action execution.
- SQL-level table-lock fault injection; prior investigation found explored SQL
  shapes are intercepted before the ownerless table-wait callback.

## Design

Add a focused `generated-column-foreign-key-policy` selector to
`packages/libmylite/tests/ownerless_cross_process_sql_test.c`.

A child ownerless process creates a valid referenced parent table and stored and
virtual generated-column child tables. After the already-open parent process
verifies those tables exist, the child attempts each rejected stored
generated-column create/alter action form and checks the expected MariaDB errno.
The child then creates a virtual generated child FK table and adds a virtual
generated child FK through `ALTER TABLE`, both with
`ON UPDATE RESTRICT ON DELETE CASCADE`.

The parent verifies that no rejected child table or constraint is visible
through `information_schema`, that the accepted virtual generated child
constraints are visible, and that missing-parent inserts, restricted parent
updates, and cascaded deletes behave through the already-open peer. Final helper
assertions reopen the database in ownerless and ordinary exclusive modes before
and after deleting volatile `.shm`.

## Compatibility Impact

This does not change SQL behavior. It records ownerless compatibility evidence
for MariaDB's generated-column foreign-key action policy and narrows the
foreign-key compatibility matrix from "planned" to "covered accepted virtual
child/rejected action forms" for these deterministic shapes.

## Directory And Lifecycle Impact

No directory-layout changes. Failed DDL must not leave durable MyLite-owned or
MariaDB-native table/constraint metadata visible in the database directory.
Accepted virtual generated child FK DDL must leave ordinary native MariaDB
metadata in the database directory. The test validates both outcomes through
MariaDB metadata and final reopen checks.

## Native Storage Impact

No native format changes. The slice relies on MariaDB/InnoDB native validation,
virtual generated-column evaluation for foreign keys, and ownerless dictionary
generation cleanup after failed DDL.

## Binary Size And Dependencies

No binary-size, dependency, or license impact.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `generated-column-foreign-key-policy` selector.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded and hook ownerless cross-process SQL CTests.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks before commit.

## Acceptance Criteria

- Stored generated child FK action clauses rejected by MariaDB fail with errno
  1905 in ownerless read/write mode.
- Virtual generated child FK create/alter attempts with
  `ON UPDATE RESTRICT ON DELETE CASCADE` are accepted in ownerless read/write
  mode and enforce missing-parent, restricted-update, and cascaded-delete
  behavior.
- Failed generated-column action DDL leaves no visible child tables or failed
  referential constraints.
- Already-open peers continue to use the valid policy tables.
- Final ownerless/native reopen before and after forced `.shm` rebuild observes
  the expected valid policy-table rows, accepted virtual child constraints, and
  no failed constraints.

## Risks And Follow-Up

- This is deterministic generated-column FK policy coverage. Deterministic
  ownerless FK graph stress is covered separately; external randomized FK graph
  stress remains separate harness work.
- Crash injection inside referential-action execution remains separate recovery
  work.
