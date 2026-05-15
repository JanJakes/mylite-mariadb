# Temporary CREATE OR REPLACE TABLE

## Goal

Cover representative `CREATE OR REPLACE TEMPORARY TABLE` behavior for MyLite:
replacing an existing temporary table should stay session-local, and creating a
temporary replacement with the same SQL name as a durable table must not drop or
rewrite the durable table.

## Non-Goals

- Full temporary-table compatibility across locks, triggers, views, partitions,
  foreign keys, unsupported indexes, or all duplicate-mode CTAS variants.
- Durable `CREATE OR REPLACE TABLE` behavior, which is covered by
  `create-or-replace-table` and `failed-create-or-replace-rollback`.
- SQL transaction, savepoint, or rollback semantics for temporary replacement.
- Public API, file-format, or dedicated non-durable temp-store changes.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_table.cc:4700-4740` checks existing temporary tables before
  base tables; if `or_replace()` is set, MariaDB drops the old temporary table
  and recreates it.
- `mariadb/sql/temporary_tables.cc:605-629` documents
  `THD::drop_temporary_table()` as the user temporary-table drop path.
- `mariadb/sql/temporary_tables.cc:645-657` rejects dropping a temporary table
  if another open instance is still in use, raising `ER_CANT_REOPEN_TABLE`.
- `mariadb/sql/temporary_tables.cc:665-686` closes temporary table handlers,
  removes the temporary share, and frees or deletes the temporary table files.
- `mariadb/sql/sql_table.cc:5768-5777` skips the non-temporary
  self-source guard for temporary `CREATE TABLE ... LIKE`.
- `mariadb/sql/sql_insert.cc:5161-5170` removes a temporary CTAS target from
  the THD temporary-table list while reading sources. This lets a new
  same-name temporary CTAS read a durable table when no old same-name temporary
  table is already shadowing it; replacing an existing same-name temporary CTAS
  target and reading that same SQL name still hits MariaDB's reopen guard.
- `mariadb/sql/sql_insert.cc:5417-5435` restores the temporary CTAS target after
  row insertion unless another inner statement created the same temporary name.

## Compatibility Impact

`CREATE OR REPLACE TABLE` remains partial, but the temporary-table branch moves
from an uncovered OR REPLACE edge case to covered representative behavior for
supported LIKE and CTAS shapes. Broader lock-table, unsupported-source,
partition, foreign-key, and transactional replacement semantics remain planned.

## Design

Use MariaDB's temporary OR REPLACE path:

1. Create durable MyLite-routed source and shadow tables.
2. Create temporary tables with the same names, then replace those temporary
   tables with `CREATE OR REPLACE TEMPORARY TABLE ... LIKE`.
3. Replace an existing temporary CTAS table whose source is a distinct durable
   table.
4. Verify SQL sees only the replacement temporary rows while the temporary
   tables exist.
5. Create a same-name temporary CTAS table over a durable source table and
   verify it does not rewrite the durable table.
6. Drop the temporary tables and verify the durable tables, rows, indexes, and
   catalog metadata remain intact.
7. Close/reopen and verify no temporary replacement survived.

No MyLite handler changes should be needed unless the temporary drop path
publishes or deletes durable catalog records.

## File Lifecycle

Temporary replacements may use MyLite temporary storage identities while open,
but they must not create durable user-schema catalog records for the temporary
SQL-visible names. They must not introduce forbidden MariaDB sidecars. Durable
same-name tables must remain in the primary `.mylite` file and become visible
again after `DROP TEMPORARY TABLE`.

## Embedded Lifecycle And API

No public API changes. The smoke test covers same-session replacement,
explicit temporary drop, close/reopen cleanup, and runtime-schema cleanup.

## Build, Size, And Dependencies

No dependency or build-profile change. Binary-size impact is limited to test
and documentation unless a handler fix is required.

## Test Plan

- Extend storage-engine smoke coverage for:
  - `CREATE OR REPLACE TEMPORARY TABLE ... LIKE` over an existing temporary
    table that shadows a durable table;
  - `CREATE OR REPLACE TEMPORARY TABLE ... AS SELECT` replacing an existing
    temporary CTAS table from a distinct durable source;
  - new same-name temporary CTAS over a durable source table;
  - MariaDB's same-name repeated temporary CTAS replacement reopen-guard
    failure, preserving the old temporary row;
  - replacement rows and indexes visible while temporary tables exist;
  - durable rows, indexes, and catalog metadata visible after
    `DROP TEMPORARY TABLE`;
  - close/reopen cleanup and sidecar gates.
- Run focused storage-smoke coverage, compatibility reports for routed DDL/DML
  and sidecar groups, format, tidy, diff, shell checks, and full preset gates.

## Acceptance Criteria

- Temporary OR REPLACE drops only an existing temporary table with that name.
- Same-name durable tables are not dropped or rewritten by temporary
  replacement.
- Temporary replacement rows are visible during the session, durable rows return
  after `DROP TEMPORARY TABLE`, and temporary names do not survive close/reopen.
- Replacing an existing same-name temporary CTAS target while reading that same
  SQL name remains documented as a MariaDB reopen-guard edge case, not hidden as
  a MyLite storage failure.
- Compatibility, roadmap, storage architecture, and OR REPLACE/temp-table specs
  identify representative temporary OR REPLACE as covered while keeping broader
  replacement semantics planned.

## Risks And Open Questions

- MariaDB's temporary OR REPLACE behavior still has lock-table and binlog
  interactions that are outside the embedded smoke profile.
- Temporary CTAS same-name source handling is covered for the durable-source
  creation shape. Replacing an existing same-name temporary CTAS table while
  reading that same SQL name hits MariaDB's reopen guard and is tracked as a
  MariaDB compatibility edge rather than a MyLite storage failure.
