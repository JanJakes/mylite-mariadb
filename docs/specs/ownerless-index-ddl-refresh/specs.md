# Ownerless Index DDL Refresh

## Problem

Ownerless DDL coverage already exercises `ALTER TABLE ... ADD/DROP INDEX`
inside broader table-shape tests, but standalone `CREATE INDEX` and `DROP
INDEX` are a separate SQL command class in MariaDB and remain a distinct
compatibility row in MyLite's matrix. MyLite needs evidence that top-level index
DDL over an InnoDB table publishes through ownerless dictionary generation,
refreshes already-open peers, leaves the table usable after index removal, and
survives no-live reopen before and after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documentation for `CREATE INDEX`
  (`https://mariadb.com/kb/v/create-index/`) describes the statement as adding
  indexes to an existing table and says it is mapped to `ALTER TABLE`.
- MariaDB documentation for `DROP INDEX`
  (`https://mariadb.com/docs/server/reference/sql-statements/data-definition/drop/drop-index`)
  describes the statement as dropping an existing index from a table and also
  maps it to `ALTER TABLE`.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_INDEX` and
  `SQLCOM_DROP_INDEX` by making re-execution-safe `Table_specification_st` and
  `Alter_info` copies, checking `INDEX_ACL`, and calling `mysql_alter_table()`.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` documents that `CREATE|DROP
  INDEX` are mapped onto the same alter-table implementation, which can use
  in-place DDL when the engine supports it.
- `mariadb/storage/innobase/handler/ha_innodb.cc` treats
  `SQLCOM_CREATE_INDEX` and `SQLCOM_DROP_INDEX` as InnoDB write DDL, including
  read-only-mode rejection and handler write-open paths.
- MyLite ownerless DDL classification in `packages/libmylite/src/database.cc`
  treats `CREATE` and `DROP` statements as dictionary DDL, so standalone index
  creation and deletion publish through the same ownerless odd/even
  dictionary-generation protocol used by table, schema, view, and trigger DDL.

## Scope And Non-Goals

- Add a focused ownerless SQL selector for standalone `CREATE INDEX` and
  `DROP INDEX`.
- Verify an already-open ownerless peer observes the index in
  `information_schema.statistics`.
- Verify the same peer can use the index with `FORCE INDEX`, insert more rows,
  and then observe the dropped index as absent.
- Verify final table rows and index absence through ownerless/native reopen
  before and after forced `.shm` rebuild.
- Do not add fulltext, spatial, unique, descending, invisible/ignored,
  multi-column, algorithm/lock-option matrix, primary-key rebuild, or crash
  recovery coverage.
- Do not add SQL-level table-lock fault injection; prior exploratory SQL shapes
  did not reach the ownerless table-wait callback.

## Design

- Add `index-ddl` to `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates an InnoDB base table, inserts three rows,
  creates a standalone secondary index over `value`, and signals the parent.
- The parent keeps an ownerless handle open, observes the index through
  `information_schema.statistics`, runs a `FORCE INDEX` query, inserts another
  row, and verifies the index remains usable over the new row.
- The child drops the index. The parent observes the index metadata absence,
  verifies `FORCE INDEX` now fails, and confirms the base table remains
  readable.
- After both ownerless handles close, helper assertions verify the final base
  table rows and index absence through:
  - `MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW`,
  - `MYLITE_OPEN_READWRITE`,
  - forced `concurrency/mylite-concurrency.shm` deletion plus ownerless reopen,
  - ordinary exclusive read/write reopen after the forced rebuild.

## Compatibility Impact

No SQL behavior changes. The compatibility matrix gains ownerless evidence for
standalone InnoDB secondary index create/use/drop refresh while keeping broader
standalone index variants and external-oracle DDL stress planned.

## Directory And Lifecycle Impact

The slice exercises existing table metadata and InnoDB index metadata inside
the MyLite-owned database directory. It adds no new MyLite files and verifies
that volatile shared-memory recreation does not lose the final index absence or
base-table rows.

## Native Storage Impact

No native storage format changes. The test exercises MariaDB/InnoDB's native
secondary-index DDL path as surfaced through standalone SQL commands.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `index-ddl` selector.
- Build and run the focused `index-ddl` selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage or focused reruns if the
  known intermittent InnoDB log-header checksum abort appears.
- Run the ownerless stress preset, `format-check`, `git diff --check`, and
  cached diff checks before commit.

## Acceptance Criteria

- Already-open ownerless peers see a standalone index created by another
  ownerless process.
- The peer can use the index with `FORCE INDEX` before it is dropped.
- Already-open peers see the index disappear after `DROP INDEX`, and forced
  index use fails while the base table remains readable.
- Final index absence and base-table rows survive ownerless/native reopen before
  and after forced `.shm` rebuild.
- Compatibility docs keep broader index variants, DDL recovery, and
  external-oracle stress gaps marked partial/planned.

## Risks And Follow-Up

- Broader standalone index variants, primary-key rebuilds, online DDL option
  matrix coverage, crash recovery during index DDL, and external randomized DDL
  stress remain outside this slice.
