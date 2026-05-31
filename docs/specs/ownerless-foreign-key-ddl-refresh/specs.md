# Ownerless Foreign Key DDL Refresh

## Problem

Ownerless DDL coverage includes foreign-key behavior for tables created with a
foreign key, but it does not yet prove `ALTER TABLE` foreign-key add/drop
refresh across already-open ownerless peers. Foreign-key ALTERs update SQL
metadata, InnoDB dictionary tables, and InnoDB dictionary cache state, and they
change user-visible write enforcement.

MyLite needs bounded evidence that an ownerless peer can observe a foreign key
added by another process, enforce it through native InnoDB checks, observe the
foreign key being dropped, and keep the final post-drop state durable through
ownerless/native reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `FOREIGN KEY` definitions in table-element
  grammar and `DROP FOREIGN KEY` in ALTER grammar.
- `mariadb/sql/handler.h` exposes `ALTER_ADD_FOREIGN_KEY` and
  `ALTER_DROP_FOREIGN_KEY` handler flags, with `DROP FOREIGN KEY` also grouped
  under `ALTER_DROP_INDEX`.
- `mariadb/sql/sql_table.cc:mysql_alter_table()` handles `ADD FOREIGN KEY IF
  NOT EXISTS`, checks existing child foreign-key names through the storage
  engine, and validates parent-table access when adding a foreign key.
- `mariadb/storage/innobase/handler/handler0alter.cc` builds InnoDB
  foreign-key alter context, writes added constraints through
  `dict_create_add_foreign_to_dictionary()`, drops constraints from
  `SYS_FOREIGN` and `SYS_FOREIGN_COLS`, and reloads foreign-key definitions
  into InnoDB's dictionary cache after ALTER commit.
- `mariadb/storage/innobase/trx/trx0trx.cc` publishes ownerless transaction
  pages and then calls `mylite_ownerless_innodb_flush_dirty_pages_to_lsn()`
  before advertising the commit as visible. FK ALTER can rebuild child-table
  clustered and secondary pages outside the normal transaction-page vector.
  Dirty-page publication must therefore run for table/index DDL commands before
  the dirty-page flush and visible-LSN publish, while ordinary DML commits must
  stay on the transaction-page vector path to avoid exposing MVCC
  history-sensitive pages through broad flush-list scans.
- MyLite's ownerless dictionary refresh runs `FLUSH TABLES` and then calls the
  InnoDB dictionary-eviction hook. InnoDB moves foreign-key tables to
  `dict_sys.table_non_LRU` through `dict_sys.prevent_eviction()`, so that hook
  must evict unused FK-bearing non-LRU dictionary entries too; otherwise a peer
  can keep enforcing a dropped FK from stale InnoDB dictionary-cache state.
- `mariadb/sql/sql_show.cc` exposes `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS`
  through the storage-engine foreign-key metadata path.
- MariaDB handler errors map missing parent rows to
  `ER_NO_REFERENCED_ROW_2` / errno 1452.

## Scope And Non-Goals

- Add a focused ownerless selector for `ALTER TABLE ... ADD CONSTRAINT ...
  FOREIGN KEY` followed by `ALTER TABLE ... DROP FOREIGN KEY`.
- Verify an already-open ownerless peer observes the added foreign key through
  `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS`.
- Verify a child row referencing a missing parent fails with MariaDB errno 1452
  while the foreign key exists.
- Verify valid child rows still succeed while the foreign key exists.
- Verify the same already-open peer observes the foreign key as absent after
  peer `DROP FOREIGN KEY`, and can then insert a child row referencing a missing
  parent.
- Verify final rows and absent-foreign-key metadata through ownerless/native
  reopen before and after forced `.shm` rebuild.
- Do not add composite foreign keys, cascading action matrices, generated-column
  foreign keys, partitioned tables, parent-row delete/update enforcement,
  concurrent FK DDL conflicts, crash recovery during FK ALTER, or SQL-level
  table-lock fault injection.

## Design

- Add `foreign-key-ddl` to `mylite_ownerless_cross_process_sql_test`.
- Publish dirty ownerless buffer-pool pages to the page-version WAL for
  InnoDB dictionary operations and table/index DDL SQL commands before waiting
  for the dirty pages to flush and publishing the ownerless visible LSN. This
  covers DDL-rebuilt FK clustered and secondary index pages that are not all
  represented in the transaction modified-page vector, without broadening
  ordinary DML commits beyond the existing transaction-page vector path.
- Skip undo-log pages during bulk dirty-page publication so MVCC reads and
  savepoint rollback continue to use native InnoDB undo-history handling.
- Relax the ownerless InnoDB dictionary-eviction hook so unused, unlocked
  FK-bearing LRU and non-LRU tables can be evicted and reloaded after peer DDL.
- A child ownerless process creates parent and child InnoDB tables without a
  foreign key, inserts valid baseline rows, then adds a named foreign key from
  `child.parent_id` to `parent.id`.
- The parent keeps an ownerless handle open, observes the added constraint in
  `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS`, verifies the pre-ALTER row
  remains visible through both the clustered primary key and child FK secondary
  index, verifies an invalid child row fails with errno 1452, and inserts a
  valid child row.
- The child drops the named foreign key. The parent observes metadata absence
  and inserts the formerly invalid child row shape.
- Final helper assertions verify row totals, the post-drop orphan row, and
  absent FK metadata through ownerless/native reopen before and after forced
  shared-memory rebuild.

## Compatibility Impact

This extends ownerless DDL evidence from create-time foreign keys to
representative `ALTER TABLE` foreign-key add/drop behavior. It does not claim a
full referential-action matrix or broad FK DDL crash/recovery coverage.

## Directory And Lifecycle Impact

The slice exercises MariaDB/InnoDB metadata and dictionary state inside the
MyLite-owned database directory. It adds no MyLite files and verifies final
state after forced volatile shared-memory rebuild.

## Native Storage Impact

No native storage format changes. The test uses MariaDB/InnoDB's native
foreign-key dictionary and enforcement paths.

## Binary Size And Dependencies

No binary-size, dependency, or license changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `foreign-key-ddl` selector.
- Build and run the focused `foreign-key-ddl` selector in
  `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage, ownerless stress, and the
  hook ownerless SQL label, using focused reruns if known intermittent ownerless
  open or InnoDB log-header checksum aborts appear.
- Run `format-check`, `git diff --check`, and cached diff checks before commit.

## Acceptance Criteria

- Already-open ownerless peers see an FK added by another ownerless process.
- Missing-parent inserts fail while the FK exists and valid inserts succeed.
- After peer `DROP FOREIGN KEY`, already-open peers see the constraint removed
  and can insert the formerly invalid child row.
- Final rows and absent-FK state survive ownerless/native reopen before and
  after forced `.shm` rebuild.
- Compatibility docs distinguish this bounded FK ALTER evidence from broader FK
  action matrices, DDL recovery, and external-oracle stress gaps.

## Risks And Follow-Up

- The `ownerless-foreign-key-actions` slice covers representative cascading,
  set-null, and restrict referential actions; composite and generated-column
  foreign-key variants remain separate DDL coverage.
- Concurrent FK DDL conflicts and crash recovery during FK ALTER remain broader
  DDL/recovery work.
