# Ownerless Table Admin Policy

## Problem

Ownerless read/write mode currently coordinates covered ordinary InnoDB DML and
DDL through directory-backed statement locks, dictionary generations,
page-version WAL, and native checkpoint evidence. MariaDB table-maintenance SQL
uses a different admin path. `OPTIMIZE TABLE` can defragment InnoDB tables and
can fall back to a recreate path; `ANALYZE TABLE` updates optimizer statistics;
`CHECK TABLE ... FOR UPGRADE` participates in upgrade workflows that may lead
to `ALTER TABLE ... FORCE`; and `REPAIR TABLE` is a storage-engine repair
surface for engines such as MyISAM and Aria.

MyLite does not yet have ownerless admin-table coordination or durable
file-lifecycle evidence for those maintenance paths. Ownerless mode must fail
closed before MariaDB enters SQL admin handlers that can update statistics,
check-and-upgrade, repair, or rebuild tables outside the ownerless dictionary
DDL protocol.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documentation describes `OPTIMIZE TABLE` as a table defragmentation
  and InnoDB full-text maintenance statement:
  `https://mariadb.com/docs/server/ha-and-performance/optimization-and-tuning/optimizing-tables/optimize-table`.
- MariaDB documentation describes `ANALYZE TABLE` as updating key distribution
  and persistent statistics:
  `https://mariadb.com/docs/server/reference/sql-statements/table-statements/analyze-table`.
- MariaDB documentation describes `CHECK TABLE` and `CHECK TABLE ... FOR
  UPGRADE` as table integrity and upgrade checks:
  `https://mariadb.com/docs/server/reference/sql-statements/table-statements/check-table`.
- MariaDB documentation describes `REPAIR TABLE` as a repair path for
  corrupted tables in supported engines:
  `https://mariadb.com/docs/server/reference/sql-statements/table-statements/repair-table`.
- `mariadb/sql/sql_yacc.yy` parses `OPTIMIZE ... TABLE` into
  `SQLCOM_OPTIMIZE` and `ALTER TABLE ... FORCE` into `ALTER_RECREATE`.
- `mariadb/sql/sql_admin.cc:mysql_admin_table()` dispatches table admin
  commands. Its repair/upgrade and optimize paths can call
  `admin_recreate_table()`.
- `mariadb/sql/sql_table.cc:mysql_recreate_table()` builds an
  `Alter_info` with `ALTER_RECREATE`, and `mysql_alter_table()` treats
  `SQLCOM_OPTIMIZE` as a copy/rebuild-sensitive path.
- `mariadb/storage/innobase/handler/handler0alter.cc` documents the shared
  `ALTER TABLE...FORCE or OPTIMIZE TABLE` rebuild handling.
- MyLite's ownerless dictionary-generation boundary currently starts for
  `ALTER`, `CREATE`, `DROP`, `RENAME`, and `TRUNCATE` statements. SQL admin
  statements are not part of that boundary.

## Scope And Non-Goals

- Reject ownerless `ANALYZE ... TABLE`.
- Reject ownerless `CHECK TABLE`.
- Reject ownerless `OPTIMIZE ... TABLE`.
- Reject ownerless `REPAIR ... TABLE`.
- Verify rejected statements leave an ordinary InnoDB table and secondary index
  usable through ownerless reopen, native exclusive reopen, and forced `.shm`
  rebuild.
- Do not implement ownerless table admin coordination, persistent-statistics
  synchronization, repair workflows, upgrade checks, or admin-triggered table
  rebuild recovery in this slice.
- Do not change ordinary exclusive read/write MariaDB behavior.

## Design

- Add an ownerless-only SQL policy predicate in
  `packages/libmylite/src/database.cc`.
- Inspect SQL policy tokens for first tokens `ANALYZE`, `CHECK`, `OPTIMIZE`,
  or `REPAIR` and reject only when a `TABLE` token appears in the statement.
- Return a MyLite policy error before MariaDB dispatch, matching the existing
  unsupported ownerless policy shape with MariaDB errno zero.
- Add a focused `table-admin-policy` ownerless SQL selector that creates an
  ordinary InnoDB table, rejects representative admin-table statements, inserts
  after rejection, and verifies the final table/index state through
  ownerless/native reopen before and after forced shared-memory rebuild.

## Compatibility Impact

Ownerless read/write mode explicitly does not support MariaDB table-maintenance
admin SQL. This is a compatibility limitation, but it is safer than allowing
admin paths that can update statistics, check upgrade state, repair engine
files, or rebuild tables without ownerless dictionary-generation and
file-lifecycle coverage.

Ordinary non-ownerless embedded behavior is unchanged.

## Directory And Lifecycle Impact

No new files or directory layout changes are introduced. The policy prevents
ownerless mode from entering unproven table maintenance and rebuild paths until
those paths have directory-owned coordination and recovery evidence.

## Native Storage Impact

No native storage format changes. Supported ordinary InnoDB tables remain
readable and writable; table admin maintenance remains future ownerless work.

## Binary Size And Dependencies

No dependency or license changes. Binary impact is limited to one SQL policy
predicate and focused test code.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `table-admin-policy` in `embedded-dev`.
- Build and run focused `table-admin-policy` in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL, ownerless
  stress, `format-check`, and diff checks.

## Acceptance Criteria

- Ownerless `ANALYZE TABLE`, `CHECK TABLE`, `OPTIMIZE TABLE`, and
  `REPAIR TABLE` fail with a MyLite policy error before MariaDB dispatch.
- Rejected statements leave the base InnoDB table, rows, and secondary index
  usable through ownerless/native reopen before and after forced `.shm`
  rebuild.
- Existing ownerless SQL, hook, and stress coverage remains green.

## Risks And Follow-Up

- Future support needs source-backed design for admin-table metadata locks,
  optimizer statistics refresh, table-rebuild integration with ownerless DDL
  generations, repair/recreate crash recovery, and external MariaDB/RQG stress.
