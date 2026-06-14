# Ownerless Stale Tablespace Cache Retry

## Problem

An already-open ownerless peer can observe SQL-layer metadata for a table that
another process created, while InnoDB still returns `ER_NO_SUCH_TABLE_IN_ENGINE`
when the peer opens that new table. The focused reproducer is the trigger DDL
refresh case: the parent sees the trigger metadata and the peer-created audit
table in `INFORMATION_SCHEMA.TABLES`, `INFORMATION_SCHEMA.INNODB_SYS_TABLES`,
`INFORMATION_SCHEMA.INNODB_SYS_COLUMNS`, and
`INFORMATION_SCHEMA.INNODB_SYS_INDEXES`, but
`INFORMATION_SCHEMA.INNODB_SYS_TABLESPACES` has no matching space and the
direct audit-table read fails with MariaDB errno `1932`.

Manual `FLUSH TABLES` before the read does not fix this state. The failing peer
has enough SQL and InnoDB dictionary metadata to name the table, but its
process-local InnoDB table and tablespace caches can still hold the failed open
state until the cache is evicted and native pages are refreshed.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/dict/dict0crea.cc:1127` through
  `dict_create_index_space()` creates a new single-table tablespace for InnoDB
  table creation.
- `mariadb/storage/innobase/fil/fil0fil.cc:2058` through
  `fil_ibd_create()` creates the file-per-table space and initializes its first
  pages through the mini-transaction/redo path before the file is opened and
  sized.
- `mariadb/storage/innobase/dict/dict0load.cc:2236` through
  `dict_load_tablespace()` loads a table's tablespace by first checking the
  process-local `fil_system` cache and then calling `fil_ibd_open()`.
- `mariadb/storage/innobase/fil/fil0fil.cc:2230` through `fil_ibd_open()`
  validates a tablespace file from the filesystem before registering a
  `fil_space_t`.
- `mariadb/storage/innobase/handler/i_s.cc:6197` scans
  `fil_system.space_list` for `INFORMATION_SCHEMA.INNODB_SYS_TABLESPACES`.
  Therefore `INNODB_SYS_TABLES` can show a table while
  `INNODB_SYS_TABLESPACES` proves that the local process still has not
  registered the file space.
- `packages/libmylite/src/database.cc:12426` starts the ownerless dictionary
  DDL generation, and `ownerless_finish_dictionary_ddl()` publishes the even
  generation that lets peers continue.

## Scope And Non-Goals

- Recover one stale InnoDB dictionary-cache miss for ownerless text plain-read
  statements by refreshing native pages, flushing SQL table cache, evicting the
  InnoDB dictionary cache, and retrying once after MariaDB errno `1932`.
- Add focused regression evidence to the existing trigger DDL refresh case by
  asserting the parent can read the peer-created audit table without a manual
  `FLUSH TABLES`, then observes it in
  `INFORMATION_SCHEMA.INNODB_SYS_TABLESPACES`.
- Keep mutating statements, DDL, transaction statements, and statements inside
  explicit transactions out of the retry path.
- Do not force a checkpoint for every ownerless `CREATE TABLE`. That was tested
  during the investigation and did not fix the stale-cache failure by itself;
  it also regressed active-reader pressure expectations by adding broad DDL
  checkpoint work.
- Keep broader DDL/file lifecycle recovery, rename/drop/truncate crash
  permutations, and external randomized stress as separate ownerless gaps.
- Do not add SQL-level table-lock fault injection; previous SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a one-shot stale-engine retry for ownerless text plain reads. If
`mysql_query()` fails with MariaDB errno `1932`, the statement is not inside an
explicit transaction, and the SQL classifier says the statement is a plain
page-version read, MyLite refreshes to the latest ownerless native LSN, runs
the same SQL/engine cache flush used for dictionary generation changes, clears
the ownerless foreign-key cache, and retries the original read once.

The retry is deliberately post-error and narrow. Mutating statements do not use
this path because a failed write may not be safe to re-execute, and DDL remains
covered by the existing ownerless dictionary generation protocol rather than a
new global checkpoint on create-table publication.

## Compatibility Impact

No SQL syntax or result semantics change. Ownerless DDL compatibility gains
stronger live-peer evidence for peer-created InnoDB tablespaces when the first
open hit a stale local cache. The behavior is still partial for the wider
DDL/file lifecycle matrix until
additional create, drop, rename, truncate, rebuild, and crash/recovery classes are
covered.

## Database Directory And Lifecycle Impact

No new files or directory layout are introduced. The slice refreshes existing
native pages and process-local dictionary caches for already-open ownerless
peers. Durable state remains inside the MyLite database directory.

## Native Storage Impact

InnoDB remains in native file-per-table format. The retry does not replace
InnoDB redo/checkpoint recovery and does not change the on-disk storage format.

## Test And Verification Plan

- Rebuild `mylite_ownerless_cross_process_sql_test` with the production
  embedded preset.
- Run the exact failing direct case:
  `mylite_ownerless_cross_process_sql_test sql-case test_ownerless_trigger_ddl_refreshes_peer_dictionary`.
- Run adjacent ownerless DDL selectors: `trigger-ddl`,
  `created-tablespace-replay`, `ctas-post-create-dml`, and
  `native-file-op-marker-drain`.
- Run the focused embedded ownerless CTest subset that includes ownerless SQL
  and primitives.
- Run ownerless stress DDL/file-lifecycle selectors when the focused fix is
  stable.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The trigger DDL refresh case no longer fails with MariaDB errno `1932` on the
  peer-created audit table.
- The already-open parent process reads the audit table without a manual
  `FLUSH TABLES`, then sees it in `INFORMATION_SCHEMA.INNODB_SYS_TABLESPACES`.
- The stale-engine retry is limited to ownerless text plain reads and does not
  retry writes, DDL, transaction statements, prepared statements, or
  explicit-transaction statements.
- Existing native file-op marker coverage still passes.
- Compatibility docs continue to mark broader ownerless DDL/file-lifecycle and
  recovery coverage as partial/planned rather than complete.

## Risks And Follow-Up

- This is a cache-recovery fix for a known stale-open failure, not a complete
  proof for every DDL file lifecycle class. Rename, drop, truncate, force
  rebuild, same-name recreation, and crash windows remain tied to the existing
  specialized tests and broader planned coverage.
- If future evidence shows a newly created tablespace needs a publication
  barrier before any peer can open it, that should be designed as a narrower
  native file-lifecycle boundary instead of a checkpoint on every
  `CREATE TABLE`.
- No-live WAL reclaim for file-per-table pages remains a separate active-reader
  pressure follow-up. A broader proof relaxation was rejected during this slice
  because it could lose commit-race updates and make independent-table stress
  readers observe decreasing totals.
- Prepared-statement stale-cache recovery remains a separate follow-up. A
  prepare retry can make `mysql_stmt_prepare()` succeed, but stepping the
  statement on an already-open handle still needs a separate visibility design.
