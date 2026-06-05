# Ownerless Primary-Key DDL Crash

## Problem Statement

Ownerless primary-key DDL coverage proves already-open peers refresh after
another ownerless process replaces an InnoDB table's primary key. It does not
yet kill a writer after MariaDB/InnoDB completes the native primary-key
replacement but before MyLite publishes ownerless dictionary finish.

This slice adds focused crash-boundary evidence for primary-key replacement,
which is a clustered-index metadata and rebuild-sensitive DDL class.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:7982` through
  `mariadb/sql/sql_yacc.yy:7985` parses `ALTER TABLE ... ADD key_def` and sets
  `ALTER_ADD_INDEX`.
- `mariadb/sql/sql_yacc.yy:8061` through
  `mariadb/sql/sql_yacc.yy:8070` parses `DROP PRIMARY KEY`, adds a `PRIMARY`
  key drop entry, and sets `ALTER_DROP_INDEX`.
- `mariadb/sql/handler.h:692` through `mariadb/sql/handler.h:696` documents the
  generic ALTER add/drop index flags used for primary-key add/drop parsing.
- `mariadb/sql/sql_table.cc:7506` through
  `mariadb/sql/sql_table.cc:7530` maps dropped and added primary keys to
  `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX` handler flags.
- `mariadb/storage/innobase/handler/handler0alter.cc:2365` through
  `mariadb/storage/innobase/handler/handler0alter.cc:2372` rejects standalone
  `DROP PRIMARY KEY` unless paired with `ADD PRIMARY KEY`.
- `mariadb/storage/innobase/handler/handler0alter.cc:4162` through
  `mariadb/storage/innobase/handler/handler0alter.cc:4223` treats a new primary
  key as a rebuild path and builds a clustered `PRIMARY` index definition.
- `mariadb/storage/innobase/handler/handler0alter.cc:6602` through
  `mariadb/storage/innobase/handler/handler0alter.cc:6614` ties the new
  clustered index definition to InnoDB's rebuild requirement.
- `mariadb/mysql-test/suite/innodb/t/alter_crash_rebuild.test:5` through
  `mariadb/mysql-test/suite/innodb/t/alter_crash_rebuild.test:23` exercises
  crash recovery around `ALTER TABLE ... ADD PRIMARY KEY`.
- `mariadb/mysql-test/suite/innodb/t/alter_copy_stats.test:36` through
  `mariadb/mysql-test/suite/innodb/t/alter_copy_stats.test:40` covers
  `DROP PRIMARY KEY, ADD PRIMARY KEY` with copy ALTER.
- `mariadb/mysql-test/suite/innodb/t/innodb-table-online.test:124` through
  `mariadb/mysql-test/suite/innodb/t/innodb-table-online.test:128` covers an
  online rebuild that drops an index and adds a primary key.

## Design

Add one unsafe-hook selector:

- initialize an ownerless database and create an InnoDB table with
  `PRIMARY KEY(id)` plus a separate unique candidate column `code`,
- start a live ownerless peer so crashed-writer cleanup remains busy,
- start a writer that executes
  `ALTER TABLE app.ownerless_primary_key_crash_base DROP PRIMARY KEY, ADD PRIMARY KEY (code)`
  under the existing `dictionary-before-finish` hook,
- kill the writer at the hook after native MariaDB/InnoDB DDL completes but
  before ownerless dictionary finish,
- prove an ownerless opener returns `MYLITE_BUSY` while the live peer remains,
- release the peer and reopen ownerless read/write to rebuild volatile
  coordination,
- verify recovered metadata exposes `PRIMARY(code)` and no longer exposes
  `PRIMARY(id)`,
- verify `FORCE INDEX (PRIMARY)` uses the new key, duplicate `code` inserts are
  rejected, duplicate `id` inserts are accepted, and later writes work,
- verify the final state survives ownerless reopen, native exclusive reopen,
  forced `.shm` rebuild, and native exclusive reopen after rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for completed primary-key
  replacement,
- live-peer cleanup-busy behavior and no-live rebuild,
- ownerless/native reopen of recovered primary-key metadata and uniqueness
  behavior.

Out of scope:

- composite, descending, AUTO_INCREMENT, generated-column, or foreign-key
  primary-key replacement crash variants,
- failed duplicate-key primary-key replacement recovery,
- randomized DDL oracle execution,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens evidence for existing
ownerless compatibility with MariaDB primary-key replacement by proving a
writer death at MyLite's dictionary publication boundary preserves the native
clustered-key metadata and uniqueness behavior.

## Directory And Lifecycle Impact

No directory layout changes are introduced. Durable state remains in the MyLite
database directory. The test exercises existing ownerless process cleanup,
dictionary-generation recovery, forced `.shm` rebuild, and native exclusive
reopen lifecycle.

## Native Storage Impact

The covered DDL uses MariaDB/InnoDB's native primary-key ALTER machinery. MyLite
does not synthesize clustered-index metadata; it proves ownerless recovery
rebuilds volatile coordination around the completed native primary-key
replacement.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-primary-key-crash`.
- Run the hook crash-tail selector.
- Run the ownerless hook SQL shards and non-hook embedded build target.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata shows `PRIMARY(code)` and no `PRIMARY(id)`.
- Duplicate `code` inserts fail with MariaDB duplicate-key errno.
- Duplicate `id` inserts and later non-conflicting writes succeed.
- Ownerless and ordinary native reopen observe the same primary-key state before
  and after forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic single-column primary-key replacement coverage, not the
  full primary-key option matrix.
- Duplicate `ADD PRIMARY KEY IF NOT EXISTS` no-op crash recovery is covered
  separately by
  `docs/specs/ownerless-primary-key-idempotent-ddl-crash/specs.md`.
- Composite, descending, AUTO_INCREMENT, generated-column, and foreign-key
  primary-key crash classes remain separate candidate slices.
- Full external MariaDB/RQG long-running stress remains planned.
