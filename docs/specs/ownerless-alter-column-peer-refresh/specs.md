# Ownerless ALTER Column Peer Refresh

## Problem

Ownerless DDL peer-refresh coverage proves cross-process visibility for
representative table creation, rename, truncate, drop, foreign keys, generated
columns, `CREATE TABLE ... LIKE`, CTAS, and online index creation. The remaining
Phase 10 DDL gap still called out broader online and column-shape ALTER classes.

This slice adds focused cross-process SQL coverage for an already-open ownerless
peer observing column-shape changes made by another ownerless process:
`ADD COLUMN`, `MODIFY COLUMN`, `CHANGE COLUMN`, and `DROP COLUMN`.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_alter.cc` implements `Sql_cmd_alter_table::execute()` and
  dispatches ALTER statements to `mysql_alter_table()`.
- `mariadb/sql/sql_table.cc` documents `mysql_alter_table()` as the common
  ALTER path for both in-place and copy algorithms. It also handles
  `CHANGE COLUMN`, `MODIFY COLUMN`, `DROP COLUMN`, and other field-list changes
  while constructing the replacement table definition.
- `mariadb/sql/handler.h` documents the online/in-place ALTER interface and
  the storage-engine callback sequence that makes engine changes durable and
  visible before the SQL-layer dictionary definition is installed.
- `packages/libmylite/src/database.cc` classifies ownerless `ALTER`, `CREATE`,
  `DROP`, `RENAME`, and `TRUNCATE` as dictionary DDL, marks a shared odd
  dictionary generation before execution, publishes the next even generation
  after execution, and makes peers wait for a stable generation before
  `FLUSH TABLES` plus InnoDB dictionary-cache eviction.
- `packages/libmylite/src/ownerless_dictionary_state.cc` stores the shared
  ownerless dictionary generation and active DDL owner in the mapped
  `concurrency/mylite-concurrency.shm` dictionary segment.

## Design

Extend the existing `test_ownerless_broader_ddl_refreshes_peer_dictionary()`
sequence instead of adding a separate slow integration test. A child ownerless
process mutates `app.ownerless_online` after the peer has already opened the
database and used the old table definition:

- add a `priority` column with a default,
- add and later drop a transient `scratch` column,
- modify the existing `status` column width/default,
- rename `status` to `state`, and
- publish the dictionary generation after each ALTER statement.

The already-open peer then queries the new table shape through ordinary SQL,
checks `information_schema.columns`, updates the renamed column and new column,
and lets the child copy the new shape through `CREATE TABLE ... LIKE` and CTAS.

## Scope And Non-Goals

In scope:

- Cross-process ownerless peer-refresh coverage for representative ALTER column
  add, modify, rename, and drop operations.
- Documentation updates narrowing the Phase 10 DDL coverage gap.

Out of scope:

- Partition ALTER, generated-column ALTER, foreign-key-rebuild ALTER, or
  DISCARD/IMPORT TABLESPACE.
- InnoDB instant-DDL-specific source claims and assertions; those are covered
  by the sibling `ownerless-instant-ddl-refresh` slice.
- Durable file-lifecycle metadata for no-live page replay of DDL-created and
  deleted tablespaces.
- External MariaDB/RQG randomized DDL oracle stress.

## Compatibility Impact

No public API or SQL policy changes are introduced. The compatibility claim is
test evidence: an already-open ownerless peer refreshes MariaDB and InnoDB
dictionary state before using a table whose columns were added, modified,
renamed, and dropped by another ownerless process.

## Directory And Lifecycle Impact

The slice does not add files or change directory layout. It exercises existing
native InnoDB table files under `datadir/` and existing ownerless dictionary
state in `concurrency/mylite-concurrency.shm`.

## Native Storage Impact

The test intentionally uses normal InnoDB ALTER TABLE paths and lets MariaDB
select in-place or copy algorithms. MyLite does not emulate table metadata or
native storage records; it verifies that the ownerless dictionary-generation
refresh is sufficient for a peer to use MariaDB's updated native table
definition.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test`.
- Run the focused `ddl-broader` selector.
- Run the embedded ownerless cross-process SQL CTest.
- Run the focused `ddl-broader` selector in the unsafe-hook preset.
- Run the ownerless stress preset.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The already-open peer observes the renamed `state` column and new `priority`
  column without reopening.
- The already-open peer no longer observes the old `status` or transient
  `scratch` columns after the child ALTER sequence.
- The peer can write through the new table shape.
- `CREATE TABLE ... LIKE` and CTAS copy the altered shape and data after the
  peer write.
- Existing ownerless DDL, crash-hook, and stress coverage remains green.

## Risks

- This remains representative coverage, not a proof for every ALTER TABLE
  operation or every InnoDB online-DDL algorithm.
- DDL/file-lifecycle metadata for broad no-live replay remains separate work.
