# Ownerless Primary Key DDL Refresh

## Problem

Ownerless DDL coverage now proves ordinary secondary-index create/drop,
multi-column unique-index enforcement/drop, and several column-shape ALTER
paths across already-open peers. Primary-key replacement is a separate InnoDB
clustered-index rebuild path: the primary key defines the clustered index, and
changing it can alter duplicate-key enforcement and physical row organization.

MyLite needs bounded ownerless evidence that MariaDB/InnoDB primary-key
replacement publishes through the existing dictionary generation protocol,
refreshes already-open peers, enforces the new primary key, and leaves final
state durable through ownerless/native reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses primary-key definitions through the ordinary
  key/constraint grammar used by `CREATE TABLE` and `ALTER TABLE`.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` marks primary-key
  replacements with `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX`.
- `mariadb/storage/innobase/handler/handler0alter.cc` explicitly rejects a
  bare InnoDB `DROP PRIMARY KEY` unless the ALTER also adds a replacement
  primary key.
- `mariadb/storage/innobase/handler/handler0alter.cc` maps replacement primary
  key metadata to `DICT_CLUSTERED | DICT_UNIQUE`, which is separate from plain
  or unique secondary-index DDL.
- `mariadb/sql/sql_show.cc` exposes primary-key metadata through
  `information_schema.statistics` with index name `PRIMARY`.
- InnoDB duplicate-key failures surface through the MariaDB handler error path
  as duplicate-key SQL errors.

## Scope And Non-Goals

- Add a focused ownerless selector for `ALTER TABLE ... DROP PRIMARY KEY, ADD
  PRIMARY KEY (...)`.
- Verify an already-open ownerless peer observes the replacement primary key in
  `information_schema.statistics` with `NON_UNIQUE = 0`.
- Verify reads can force the replacement `PRIMARY` index through the new key
  column.
- Verify duplicate writes against the new primary key fail with MariaDB
  duplicate-key errno.
- Verify writes that duplicate only the old primary-key column succeed after
  replacement.
- Verify final rows and replacement primary-key metadata through ownerless and
  native reopen before and after forced `.shm` rebuild.
- Do not add unsupported bare `DROP PRIMARY KEY` coverage; MariaDB/InnoDB
  requires replacement primary-key creation in the same ALTER.
- Do not add composite primary-key, descending key, algorithm/lock option
  matrix, crash recovery during primary-key rebuild, or concurrent
  duplicate-key race coverage. AUTO_INCREMENT primary-key replacement is
  covered separately by `ownerless-autoinc-primary-key-ddl-refresh`.
- Do not add SQL-level table-lock fault injection; prior exploratory SQL shapes
  did not reach the ownerless table-wait callback.

## Design

- Add `primary-key-ddl` to `mylite_ownerless_cross_process_sql_test`.
- A child ownerless process creates an InnoDB table with primary key `(id)`,
  inserts distinct rows, then runs `ALTER TABLE ... DROP PRIMARY KEY, ADD
  PRIMARY KEY (code)`.
- The parent keeps an ownerless handle open, observes the replacement primary
  key through `information_schema.statistics`, verifies forced-index reads by
  `code`, verifies a duplicate `code` insert fails with MariaDB errno 1062, and
  inserts a row that reuses the old `id` with a new `code`.
- Final helper assertions verify row totals, old-key duplicate presence,
  replacement primary-key metadata, and forced-index reads through
  ownerless/native reopen before and after forced shared-memory rebuild.

## Compatibility Impact

This extends ownerless DDL evidence from secondary index changes to a
representative InnoDB clustered-index replacement. It does not claim broad
primary-key option coverage or support for unsupported MariaDB/InnoDB bare
primary-key drops.

## Directory And Lifecycle Impact

The slice exercises existing MariaDB/InnoDB dictionary and clustered-index
metadata inside the MyLite-owned database directory. It adds no MyLite files and
verifies final state after forced volatile shared-memory rebuild.

## Native Storage Impact

No native storage format changes. The test uses MariaDB/InnoDB's native
primary-key replacement path and duplicate-key enforcement.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `primary-key-ddl` selector.
- Build and run the focused `primary-key-ddl` selector in
  `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage, ownerless stress, and the
  hook ownerless SQL label, using focused reruns if the known intermittent
  InnoDB log-header checksum abort appears.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Already-open ownerless peers see a replacement primary key created by another
  ownerless process.
- Forced-index reads use the replacement primary key.
- Duplicate inserts fail for the replacement key and succeed for values that
  duplicate only the old primary-key column.
- Final rows and replacement primary-key state survive ownerless/native reopen
  before and after forced `.shm` rebuild.
- Compatibility docs distinguish this bounded primary-key replacement evidence
  from broader DDL, recovery, option-matrix, and external-oracle stress gaps.

## Risks And Follow-Up

- Concurrent conflicting primary-key replacements remain a broader DDL stress
  and external-oracle class.
- Composite, descending, invisible/ignored, and algorithm/lock option variants
  remain planned. AUTO_INCREMENT primary-key replacement is covered separately
  by `ownerless-autoinc-primary-key-ddl-refresh`.
