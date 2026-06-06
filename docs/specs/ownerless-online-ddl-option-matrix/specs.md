# Ownerless Online DDL Option Matrix

## Problem Statement

Ownerless DDL coverage already proves peer dictionary refresh for representative
online and in-place index operations, instant column operations, and copy-style
rebuilds. The cross-process concurrency spec still leaves broader accepted
online DDL option combinations as planned. This slice narrows that gap for
ordinary InnoDB secondary indexes by covering explicit `NOCOPY`/`LOCK=SHARED`,
`NOCOPY`/`LOCK=EXCLUSIVE`, and `INPLACE`/`LOCK=EXCLUSIVE` add/drop paths under
the existing ownerless peer refresh, reopen, and forced shared-memory rebuild
checks.

Non-goals:

- enable unsupported `FULLTEXT`, `SPATIAL`, partition, table-directory, or
  tablespace detach/import DDL in ownerless mode,
- prove SQL-level table-lock wait fault injection for native table-wait paths,
- replace the existing external MariaDB/RQG follow-up with randomized DDL
  execution.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_alter.cc:70` through `mariadb/sql/sql_alter.cc:98` parse
  `ALGORITHM=NOCOPY`, `ALGORITHM=INSTANT`, `LOCK=NONE`, `LOCK=SHARED`,
  `LOCK=EXCLUSIVE`, and `LOCK=DEFAULT` into `Alter_info`.
- `mariadb/sql/sql_alter.cc:138` through `mariadb/sql/sql_alter.cc:218`
  validate requested algorithm and lock clauses against the handler's reported
  in-place capability.
- `mariadb/sql/sql_table.cc:11589` through
  `mariadb/sql/sql_table.cc:11713` route non-copy ALTER through
  `fill_alter_inplace_info()`, create an altered table definition, ask the
  handler for support, and then enforce the requested algorithm and lock.
- `mariadb/storage/innobase/handler/handler0alter.cc:1687` through
  `mariadb/storage/innobase/handler/handler0alter.cc:1729` decide whether an
  InnoDB column ALTER can be instant.
- `mariadb/storage/innobase/handler/handler0alter.cc:2681` through
  `mariadb/storage/innobase/handler/handler0alter.cc:2724` returns
  `HA_ALTER_INPLACE_INSTANT` when an instant operation is supported.
- Upstream MariaDB tests exercise accepted option combinations, including
  ordinary `ADD INDEX` with `ALGORITHM=INPLACE` and `LOCK=NONE`/`SHARED`/
  `EXCLUSIVE` in `mariadb/mysql-test/main/alter_table.test:1513` through
  `mariadb/mysql-test/main/alter_table.test:1515`, and instant column
  variations in `mariadb/mysql-test/suite/innodb/t/instant_alter.test:434`
  through `mariadb/mysql-test/suite/innodb/t/instant_alter.test:462`.

## Design

Extend `test_ownerless_online_ddl_options_refresh_peer_dictionary()` rather
than introducing a new harness. The existing selector already:

- opens one ownerless DDL process and one already-open ownerless peer,
- synchronizes each DDL boundary through pipes,
- verifies the peer sees dictionary and optimizer-visible metadata changes
  immediately after each boundary,
- verifies final state through ownerless reopen, ordinary native exclusive
  reopen, forced `.shm` rebuild, and native exclusive reopen after rebuild.

Add six DDL stages:

1. `ADD INDEX ... ALGORITHM=NOCOPY, LOCK=SHARED`
2. `DROP INDEX ... ALGORITHM=NOCOPY, LOCK=SHARED`
3. `ADD INDEX ... ALGORITHM=NOCOPY, LOCK=EXCLUSIVE`
4. `DROP INDEX ... ALGORITHM=NOCOPY, LOCK=EXCLUSIVE`
5. `ADD INDEX ... ALGORITHM=INPLACE, LOCK=EXCLUSIVE`
6. `DROP INDEX ... ALGORITHM=INPLACE, LOCK=EXCLUSIVE`

The peer checks both `INFORMATION_SCHEMA.STATISTICS` and `FORCE INDEX`
behavior at each add/drop boundary. Final-state assertions verify both
temporary indexes are absent after every reopen path.

## Compatibility Impact

This slice does not add a new SQL feature. It strengthens the evidence for
accepted MariaDB online DDL option combinations in ownerless read/write mode.
Unsupported ownerless DDL classes remain explicitly rejected and unchanged.

## Directory And Lifecycle Impact

The slice adds no durable files and no directory layout change. It reuses native
InnoDB secondary-index DDL inside `datadir/app/*.ibd`, the existing ownerless
dictionary-generation boundary, and the existing reopen and forced `.shm`
rebuild checks.

## Native Storage Impact

No native storage format changes are made. The covered operations rely on
MariaDB/InnoDB's existing online DDL machinery. MyLite proves that the ownerless
peer refresh path observes the resulting dictionary/index metadata across the
selected option combinations.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with the embedded preset.
- Run the focused selector:
  `build/embedded-dev/packages/libmylite/mylite_ownerless_cross_process_sql_test online-ddl-options`.
- Run the embedded ownerless cross-process SQL CTest label.
- Run `cmake --build --preset embedded-dev --target format-check`.
- Run `git diff --check`.

## Acceptance Criteria

- The focused selector proves peer-visible add/drop metadata for the new
  `NOCOPY`/`LOCK=SHARED`, `NOCOPY`/`LOCK=EXCLUSIVE`, and
  `INPLACE`/`LOCK=EXCLUSIVE` option combinations.
- Final ownerless/native reopen checks, including forced `.shm` rebuild, prove
  no stale transient index metadata survives.
- Compatibility docs and the ownerless concurrency spec name the new coverage
  and keep external randomized DDL oracles as planned.

## Risks And Unresolved Questions

- This remains a deterministic selector, not randomized DDL exploration.
- SQL-level table-lock wait fault injection remains planned because existing
  SQL shapes have not reached the ownerless table-wait callback.
- Full external MariaDB/RQG long-running DDL oracle execution remains
  environment-owned follow-up work.
