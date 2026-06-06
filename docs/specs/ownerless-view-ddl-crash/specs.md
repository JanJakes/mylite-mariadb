# Ownerless View DDL Crash

## Problem Statement

Ownerless view refresh coverage proves already-open peers observe simple
`CREATE VIEW`, query through the view over an InnoDB base table, and later
observe `DROP VIEW`. The crash boundary still needs focused evidence: if a
writer dies after MariaDB creates or removes the native view definition file
but before MyLite publishes ownerless dictionary finish, no-live recovery must
preserve the completed view metadata state.

This slice adds deterministic crash-boundary evidence for simple view
CREATE/DROP DDL.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_parse.cc:5761` through
  `mariadb/sql/sql_parse.cc:5768` dispatches `SQLCOM_CREATE_VIEW` to
  `mysql_create_view()`.
- `mariadb/sql/sql_parse.cc:5770` through
  `mariadb/sql/sql_parse.cc:5782` dispatches `SQLCOM_DROP_VIEW` to
  `mysql_drop_view()`.
- `mariadb/sql/sql_view.cc:401` through `mariadb/sql/sql_view.cc:449`
  implements `mysql_create_view()`, rejects view DDL under `LOCK TABLES`, and
  runs create-view prechecks before definition-file publication.
- `mariadb/sql/sql_view.cc:1224` through `mariadb/sql/sql_view.cc:1248`
  records DDL-log state and writes the view definition file through
  `sql_create_definition_file()`.
- `mariadb/sql/sql_view.cc:1919` through `mariadb/sql/sql_view.cc:1986`
  implements `mysql_drop_view()`, locks view names, validates the target `.frm`
  as a view, records DDL-log state, and deletes the view definition file.
- `mariadb/sql/sql_table.cc:1339` through
  `mariadb/sql/sql_table.cc:1344` exposes the `drop_view` path in
  `mysql_rm_table_no_locks()`, and `mariadb/sql/sql_table.cc:1691` through
  `mariadb/sql/sql_table.cc:1694` records dropped-view DDL-log state when a
  generic drop path handles a view.
- `mariadb/sql/sql_show.cc:7515` through
  `mariadb/sql/sql_show.cc:7527` populates information-schema view records
  from opened view metadata.
- `mariadb/sql/sql_show.cc:10961` through
  `mariadb/sql/sql_show.cc:10963` exposes `INFORMATION_SCHEMA.VIEWS`.
- `packages/libmylite/src/database.cc:8637` through
  `packages/libmylite/src/database.cc:8675` treats `CREATE` and `DROP`
  statements as ownerless dictionary DDL, so simple view creation and deletion
  run through the same odd/even dictionary-generation protocol and unsafe
  `dictionary-before-finish` hook as table DDL.

## Design

Add two unsafe-hook selectors:

- `dictionary-view-create-crash` initializes an ownerless database with an
  InnoDB base table, verifies the target view is absent, keeps a live ownerless
  peer open, kills a writer after `CREATE VIEW` completes natively but before
  ownerless dictionary finish, verifies live-peer cleanup remains busy, then
  reopens no-live ownerless and checks the recovered view metadata and query
  behavior.
- `dictionary-view-drop-crash` initializes an ownerless database with an InnoDB
  base table plus a simple view, verifies the view is present and queryable,
  keeps a live ownerless peer open, kills a writer after `DROP VIEW` completes
  natively but before ownerless dictionary finish, verifies live-peer cleanup
  remains busy, then reopens no-live ownerless and checks recovered view
  absence while the base table remains writable.

Both selectors verify ownerless and ordinary native reopen before and after
forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for simple `CREATE VIEW` and
  `DROP VIEW`,
- recovered present/absent `INFORMATION_SCHEMA.VIEWS` metadata,
- recovered `.frm` presence or absence under `datadir/app/`,
- view query behavior after recovered create and base-table writes after
  recovered drop,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `CREATE VIEW IF NOT EXISTS`, `DROP VIEW IF EXISTS`, nested views,
  check-option views, invalid dependency handling, security/definer semantics,
  and updatable-view crash variants,
- trigger and stored-routine crash coverage,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless view
compatibility evidence by proving completed simple CREATE/DROP view metadata
survives a writer death at MyLite's dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test exercises MariaDB native
view definition files under the MyLite-owned `datadir/app/` directory,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

The view metadata is MariaDB-native SQL-layer metadata. The base table is InnoDB
so the selector also checks base-table durability and post-recovery DML, but it
does not alter InnoDB storage formats or page-version replay policy.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-view-drop-crash`
- Run the normal embedded `view-ddl` selector.
- Run the hook crash-tail selector and ownerless hook SQL shards.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- CREATE recovery exposes the view through `INFORMATION_SCHEMA.VIEWS`,
  preserves the `.frm` file, and allows queries through the view.
- DROP recovery removes the view from `INFORMATION_SCHEMA.VIEWS`, removes the
  `.frm` file, rejects view queries, and keeps the base table writable.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic simple view CREATE/DROP crash coverage, not the full
  view crash matrix.
- Replacement/alter crash coverage is tracked by
  `docs/specs/ownerless-view-replacement-ddl-crash/specs.md`; idempotent
  no-op crash coverage is tracked by
  `docs/specs/ownerless-view-idempotent-ddl-crash/specs.md`; check-option and
  nested check-option crash coverage are tracked by their focused specs, while
  security/definer, broader nested-view, and updatable-view crash variants
  remain separate slices.
- Trigger crash recovery and full external MariaDB/RQG long-running DDL stress
  remain planned.
