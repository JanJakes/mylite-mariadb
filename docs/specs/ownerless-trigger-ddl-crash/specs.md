# Ownerless Trigger DDL Crash

## Problem Statement

Ownerless trigger refresh coverage proves already-open peers observe simple
`CREATE TRIGGER`, fire the trigger through base-table DML, and later observe
`DROP TRIGGER`. The crash boundary still needs focused evidence: if a writer
dies after MariaDB creates or removes native trigger metadata but before MyLite
publishes ownerless dictionary finish, no-live recovery must preserve the
completed trigger metadata state.

This slice adds deterministic crash-boundary evidence for simple trigger
CREATE/DROP DDL.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_parse.cc:5784` through
  `mariadb/sql/sql_parse.cc:5798` dispatches `SQLCOM_CREATE_TRIGGER` and
  `SQLCOM_DROP_TRIGGER` to `mysql_create_or_drop_trigger()`.
- `mariadb/sql/sql_trigger.cc:418` through
  `mariadb/sql/sql_trigger.cc:670` implements the shared trigger DDL path: it
  acquires an exclusive trigger metadata lock, opens the subject base table,
  waits while the table is used, and routes create/drop through
  `Table_triggers_list`.
- `mariadb/sql/sql_trigger.cc:1007` through
  `mariadb/sql/sql_trigger.cc:1015` builds the table-level `.TRG` path and
  trigger-name `.TRN` path inside the database directory.
- `mariadb/sql/sql_trigger.cc:1100` through
  `mariadb/sql/sql_trigger.cc:1143` creates the `.TRN` file, populates the
  in-memory trigger object, and writes the table-level `.TRG` definition file.
- `mariadb/sql/sql_trigger.cc:1403` through
  `mariadb/sql/sql_trigger.cc:1459` drops a trigger by deleting it from the
  trigger list, removing or rewriting the table-level `.TRG` file, and removing
  the trigger-name `.TRN` file.
- `packages/libmylite/src/database.cc:8672` through
  `packages/libmylite/src/database.cc:8674` classifies `CREATE` and `DROP`
  statements as ownerless dictionary DDL.
- `packages/libmylite/src/database.cc:9299` through
  `packages/libmylite/src/database.cc:9305` runs the unsafe
  `dictionary-before-finish` hook after native SQL execution but before the
  ownerless dictionary finish transition.

## Design

Add two unsafe-hook selectors:

- `dictionary-trigger-create-crash` initializes an ownerless database with
  InnoDB base and audit tables, verifies the target trigger and native metadata
  files are absent, keeps a live ownerless peer open, kills a writer after
  `CREATE TRIGGER` completes natively but before ownerless dictionary finish,
  verifies live-peer cleanup remains busy, then reopens no-live ownerless and
  checks recovered trigger metadata and trigger firing.
- `dictionary-trigger-drop-crash` initializes an ownerless database with
  InnoDB base and audit tables plus a simple `AFTER INSERT` trigger, verifies
  the trigger is present and fires, keeps a live ownerless peer open, kills a
  writer after `DROP TRIGGER` completes natively but before ownerless
  dictionary finish, verifies live-peer cleanup remains busy, then reopens
  no-live ownerless and checks recovered trigger absence while the base table
  remains writable and later inserts do not fire the dropped trigger.

Both selectors verify ownerless and ordinary native reopen before and after
forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for simple `CREATE TRIGGER` and
  `DROP TRIGGER`,
- recovered present/absent `INFORMATION_SCHEMA.TRIGGERS` metadata,
- recovered `.TRG` and `.TRN` presence or absence under `datadir/app/`,
- trigger firing after recovered create and non-firing after recovered drop,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- `CREATE OR REPLACE TRIGGER`, `CREATE TRIGGER IF NOT EXISTS`,
  `DROP TRIGGER IF EXISTS`, multi-trigger ordering, `SHOW TRIGGERS`,
  definer/security semantics, invalid dependency handling, trigger bodies that
  call stored functions, and randomized trigger oracle coverage,
- stored-routine ownerless support,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless
trigger compatibility evidence by proving completed simple CREATE/DROP trigger
metadata survives a writer death at MyLite's dictionary publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The test exercises MariaDB native
trigger metadata files under the MyLite-owned `datadir/app/` directory,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

Trigger metadata is MariaDB-native SQL-layer metadata. The base and audit
tables are InnoDB, so the selectors also check base-table durability,
post-recovery DML, and trigger-maintained audit rows, but they do not alter
InnoDB storage formats or page-version replay policy.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-trigger-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-trigger-drop-crash`
- Run the normal embedded `trigger-ddl` selector.
- Run the hook crash-tail selector and ownerless hook SQL shards.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- CREATE recovery exposes the trigger through `INFORMATION_SCHEMA.TRIGGERS`,
  preserves the `.TRG` and `.TRN` files, and fires the trigger through
  base-table inserts.
- DROP recovery removes the trigger from `INFORMATION_SCHEMA.TRIGGERS`,
  removes the `.TRG` and `.TRN` files, rejects `SHOW CREATE TRIGGER`, and keeps
  later base-table inserts from updating the audit table.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic simple trigger CREATE/DROP crash coverage, not the full
  trigger crash matrix.
- Replacement, idempotent, ordering, security/definer, invalid dependency,
  stored-function, and randomized trigger crash variants remain separate
  candidate slices.
- Full external MariaDB/RQG long-running DDL stress remains planned.
