# Ownerless Unique Index DDL Refresh

## Problem

Ownerless standalone index coverage proves ordinary secondary-index
create/use/drop refresh across already-open peers. It deliberately left unique
and multi-column index variants outside scope. Unique indexes add a user-visible
write invariant: duplicate keys must be rejected while the index exists, and the
same duplicate shape must become legal after the index is dropped.

MyLite needs bounded ownerless evidence that MariaDB/InnoDB unique index DDL
publishes through the existing dictionary generation protocol, refreshes
already-open peers, preserves uniqueness enforcement before drop, and leaves the
final post-drop state durable through ownerless/native reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB `CREATE INDEX` documentation
  (`https://mariadb.com/kb/v/create-index/`) includes `UNIQUE` in the top-level
  index syntax and states that `CREATE INDEX` maps to `ALTER TABLE`.
- `mariadb/sql/sql_yacc.yy` parses ordinary index definitions through
  `constraint_key_type` and `key_list`, which covers unique and multi-column
  index definitions in `CREATE TABLE`, `ALTER TABLE`, and `CREATE INDEX`.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_INDEX` /
  `SQLCOM_DROP_INDEX` by checking `INDEX_ACL` and calling
  `mysql_alter_table()`.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` documents that
  `CREATE|DROP INDEX` use the same alter-table path.
- `mariadb/storage/innobase/handler/handler0alter.cc` has explicit
  `ALTER_ADD_UNIQUE_INDEX` and `ALTER_DROP_UNIQUE_INDEX` handling, and maps
  unique key flags to `DICT_UNIQUE` metadata.
- `mariadb/sql/sql_show.cc` exposes unique index metadata through
  `information_schema.statistics.NON_UNIQUE`.
- InnoDB duplicate-key failures surface through the MariaDB handler error path
  as duplicate-key SQL errors.

## Scope And Non-Goals

- Add a focused ownerless selector for standalone multi-column
  `CREATE UNIQUE INDEX` and `DROP INDEX`.
- Verify an already-open ownerless peer observes a two-column unique index in
  `information_schema.statistics` with `NON_UNIQUE = 0`.
- Verify duplicate writes fail with MariaDB duplicate-key errno while the index
  exists.
- Verify unique non-duplicate writes still succeed through the already-open
  peer.
- Verify dropping the unique index refreshes the peer so forced-index use fails
  and a duplicate key shape can be inserted.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.
- Do not add primary-key rebuild, descending, invisible/ignored, algorithm/lock
  option matrix, special full-text/spatial indexes, or crash recovery during
  unique-index DDL.
- Do not add SQL-level table-lock fault injection; prior exploratory SQL shapes
  did not reach the ownerless table-wait callback.

## Design

- Add `unique-index-ddl` to `mylite_ownerless_cross_process_sql_test`.
- A child ownerless process creates an InnoDB table with `(tenant_id, slug)`
  business keys, inserts distinct rows, creates a standalone unique index on
  `(tenant_id, slug)`, and signals the parent.
- The parent keeps an ownerless handle open, observes the unique two-column
  index through `information_schema.statistics`, verifies forced-index reads,
  verifies a duplicate insert fails with MariaDB errno 1062, and inserts a
  non-conflicting row.
- The child drops the unique index. The parent observes index absence, verifies
  `FORCE INDEX` fails, then inserts the formerly duplicate key shape.
- Final helper assertions verify row totals, duplicate-key shape presence, and
  index absence through ownerless/native reopen before and after forced shared
  memory rebuild.

## Compatibility Impact

This extends ownerless index DDL evidence from plain secondary indexes to a
representative multi-column unique index. It does not claim broad index option
coverage or special-index support.

## Directory And Lifecycle Impact

The slice exercises existing MariaDB/InnoDB dictionary and index metadata inside
the MyLite-owned database directory. It adds no MyLite files and verifies final
state after forced volatile shared-memory rebuild.

## Native Storage Impact

No native storage format changes. The test uses MariaDB/InnoDB's native unique
secondary-index DDL and duplicate-key enforcement.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `unique-index-ddl` selector.
- Build and run the focused `unique-index-ddl` selector in
  `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage, ownerless stress, and the
  hook ownerless SQL label, using focused reruns if the known intermittent
  InnoDB log-header checksum abort appears.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Already-open ownerless peers see a multi-column unique index created by
  another ownerless process.
- Duplicate inserts fail while the unique index exists and non-conflicting
  inserts still succeed.
- After peer `DROP INDEX`, already-open peers see the index disappear and can
  insert the formerly duplicate key shape.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.
- Compatibility docs distinguish this bounded unique-index evidence from broad
  index option and external-oracle stress gaps.

## Risks And Follow-Up

- Concurrent duplicate-key races over the same unique key remain a separate
  stress/oracle class.
- Primary-key rebuild, invisible/ignored indexes, descending indexes, algorithm
  matrices, and crash recovery during index DDL remain planned.
