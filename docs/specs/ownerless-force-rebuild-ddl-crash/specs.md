# Ownerless Force-Rebuild DDL Crash

## Problem Statement

Ownerless DDL coverage proves peer-visible `ALTER TABLE ... FORCE` rebuilds and
retained-WAL replay for force-rebuilt file-per-table spaces. It does not yet
kill an ownerless writer after MariaDB/InnoDB completes the native force rebuild
but before MyLite publishes ownerless dictionary finish.

This slice adds focused crash-boundary evidence for that native rebuild/file
lifecycle path.

Supersession note: the later
`docs/specs/ownerless-live-force-rebuild-recovery/specs.md` slice upgrades the
same focused `ALTER TABLE ... FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE` selector
from no-live-only recovery to live-peer dictionary recovery. This spec remains
historical evidence for the force-rebuild crash boundary.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:8155` through
  `mariadb/sql/sql_yacc.yy:8158` parses `ALTER TABLE ... FORCE` by setting
  `ALTER_RECREATE`.
- `mariadb/sql/handler.h:707` through `mariadb/sql/handler.h:708` documents
  `ALTER_RECREATE` as the flag for `FORCE`, same-engine `ENGINE`, and
  `mysql_recreate_table()`.
- `mariadb/sql/sql_table.cc:11402` through
  `mariadb/sql/sql_table.cc:11420` treats same-engine ALTER and standalone
  `FORCE` as table-rebuild requests and marks standalone `FORCE` as an
  identical-table recreate path.
- `mariadb/sql/sql_table.cc:11423` through
  `mariadb/sql/sql_table.cc:11446` documents the copy-algorithm fallback and
  data-conversion cases for ALTER rebuilds.
- `mariadb/storage/innobase/handler/handler0alter.cc:7237` through
  `mariadb/storage/innobase/handler/handler0alter.cc:7245` handles
  `ALTER TABLE ... FORCE` and `OPTIMIZE TABLE` as user-visible no-schema-change
  ALTER paths in InnoDB alter handling.
- `mariadb/mysql-test/main/innodb_mysql_sync.test:604` through
  `mariadb/mysql-test/main/innodb_mysql_sync.test:617` covers
  `ALTER TABLE FORCE` as online rebuild and `ALTER TABLE FORCE, ALGORITHM=COPY`
  as table-copy rebuild.
- `mariadb/mysql-test/suite/innodb/t/alter_copy.test:39` through
  `mariadb/mysql-test/suite/innodb/t/alter_copy.test:44` uses
  `ALTER TABLE ... FORCE, ALGORITHM=COPY` in an InnoDB copy-alter crash-style
  synchronization test.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database and create an indexed InnoDB table with
  payload bytes large enough to verify copied row contents,
- start a live ownerless peer so crashed-writer cleanup remains busy,
- start a writer that executes
  `ALTER TABLE app.ownerless_force_rebuild_crash_base FORCE, ALGORITHM=COPY, LOCK=EXCLUSIVE`
  under the existing `dictionary-before-finish` hook,
- kill the writer at the hook after native MariaDB/InnoDB DDL has completed but
  before ownerless dictionary finish,
- originally prove an ownerless opener returns `MYLITE_BUSY` while the live
  peer remains; the later live-force-rebuild slice supersedes that expectation
  for this exact statement by allowing live-peer dictionary recovery,
- release the peer and reopen ownerless read/write to drain the native marker
  and rebuild volatile coordination,
- verify the recovered table has InnoDB table/space metadata, the secondary
  index is usable through `FORCE INDEX`, copied row payloads remain intact, and
  post-recovery writes succeed,
- verify the final state survives ownerless reopen, native exclusive reopen,
  forced `.shm` rebuild, and native exclusive reopen after rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for a completed
  `ALTER TABLE ... FORCE, ALGORITHM=COPY` rebuild,
- original live-peer cleanup-busy behavior and no-live rebuild; the later
  live-force-rebuild slice supersedes the busy expectation for this focused
  statement shape,
- ownerless/native reopen of recovered table, InnoDB space metadata, secondary
  index metadata, copied payloads, and post-recovery writes.

Out of scope:

- randomized DDL oracle execution,
- partitioned or external-directory table rebuilds,
- `FULLTEXT`/`SPATIAL` rebuilds,
- table import/discard tablespace lifecycle,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens evidence for the existing
ownerless `ALTER TABLE ... FORCE` compatibility claim by proving that a writer
death at MyLite's dictionary publication boundary does not leave native rebuild
state unusable.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test exercises existing
file-per-table `.frm` and `.ibd` lifecycle inside the MyLite database directory,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen.

## Native Storage Impact

The covered DDL uses MariaDB/InnoDB's native copy ALTER machinery. MyLite does
not reinterpret the rebuilt table or index contents; it proves no-live
ownerless recovery rebuilds volatile coordination around the completed native
rebuild.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-force-rebuild-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- In the original slice, a live peer prevents cleanup until no-live recovery;
  the later live-force-rebuild slice supersedes this for the exact focused
  statement shape.
- Recovered InnoDB table/space metadata is present.
- The secondary index is visible and usable after recovery.
- Copied payload bytes and aggregate row values survive recovery.
- Post-recovery writes succeed.
- Ownerless and ordinary native reopen observe the same rebuilt table state
  before and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic copy-rebuild coverage, not exhaustive ALTER FORCE
  algorithm exploration.
- Broader file-lifecycle classes such as partitioned tables, tablespace import,
  special indexes, and external directories remain explicitly out of scope or
  unsupported.
- Full external MariaDB/RQG long-running stress remains planned.
