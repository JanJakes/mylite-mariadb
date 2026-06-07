# Ownerless Trigger Invalid Dependency Crash

## Problem Statement

Ownerless trigger crash coverage proves completed trigger create, drop,
replacement, ordering, and idempotent no-op metadata survives a writer death
after MariaDB native trigger file changes but before MyLite publishes ownerless
dictionary finish. One remaining trigger edge is a valid MariaDB trigger whose
body references an object that does not exist yet.

MariaDB accepts that trigger definition and reports the missing dependency when
the trigger fires. MyLite must recover the native `.TRG`/`.TRN` metadata after
a crash at the ownerless dictionary boundary without losing the trigger or
pretending the missing dependency exists.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses `CREATE TRIGGER` bodies as stored-program
  statements and does not require every referenced table to exist at creation.
- `mariadb/sql/sql_trigger.cc:657` through
  `mariadb/sql/sql_trigger.cc:667` dispatches trigger create/drop through
  `Table_triggers_list::create_trigger()` or
  `Table_triggers_list::drop_trigger()`.
- `mariadb/sql/sql_trigger.cc:926` through
  `mariadb/sql/sql_trigger.cc:1143` writes the trigger-name `.TRN` file and
  table-level `.TRG` file for accepted trigger definitions.
- `mariadb/sql/table.cc` and DML execution paths call
  `Table_triggers_list::process_triggers()`, so missing objects referenced by
  trigger bodies surface when the trigger body executes.
- `packages/libmylite/src/database.cc` starts ownerless dictionary DDL for
  ownerless `CREATE` statements, then the unsafe `dictionary-before-finish`
  hook can kill the writer after MariaDB native SQL execution but before
  ownerless dictionary finish publication.

## Design

Add a hook-only selector,
`dictionary-trigger-invalid-dependency-crash`, to
`mylite_ownerless_cross_process_sql_test`:

1. Create an InnoDB base table without creating the trigger body's audit table.
2. Keep a live ownerless peer open.
3. Kill a writer after `CREATE TRIGGER` writes native `.TRG`/`.TRN` metadata
   for an `AFTER INSERT` trigger whose body inserts into the missing audit
   table, but before ownerless dictionary finish.
4. Prove the live peer prevents dead-writer cleanup until a no-live ownerless
   reopen performs recovery.
5. Verify the recovered trigger metadata is visible through
   `INFORMATION_SCHEMA.TRIGGERS`, `.TRG`/`.TRN` files, and
   `SHOW CREATE TRIGGER`.
6. Verify firing the recovered trigger before the audit table exists fails with
   MariaDB `ER_NO_SUCH_TABLE` (`1146`) and does not insert the base row.
7. Create the missing audit table, fire the same recovered trigger, and verify
   base and audit rows survive ownerless and ordinary native reopen before and
   after forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for a trigger body that references
  a table absent at trigger creation time,
- recovered `.TRG` and `.TRN` files under `datadir/app/`,
- `INFORMATION_SCHEMA.TRIGGERS` and `SHOW CREATE TRIGGER` verification,
- MariaDB-compatible delayed missing-table error when the trigger fires,
- post-dependency trigger firing after the missing audit table is created,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- broader privilege/security semantics,
- trigger bodies that call stored functions,
- randomized trigger crash oracle coverage,
- enabling ownerless stored routine execution,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless trigger
compatibility evidence by proving MyLite preserves MariaDB's accepted delayed
dependency semantics through a crash at MyLite's dictionary publication
boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The selector exercises native
MariaDB trigger metadata files under the MyLite-owned database directory:

- `datadir/app/ownerless_trigger_invalid_dependency_crash_base.TRG`,
- `datadir/app/ownerless_trigger_invalid_dependency_crash_ai.TRN`,
- delayed creation of
  `datadir/app/ownerless_trigger_invalid_dependency_crash_audit.ibd`.

## Native Storage Impact

Trigger metadata is MariaDB-native SQL-layer metadata. The base and delayed
audit table are InnoDB, so the selector also verifies post-recovery DML and
native tablespace persistence, but it does not change InnoDB file formats,
page-version replay, redo/checkpoint policy, or file lifecycle routing.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-trigger-invalid-dependency-crash`
- Run adjacent hook trigger crash selectors and `crash-tail`.
- Run ownerless SQL CTest filters, ownerless stress, `format-check`, `tidy`,
  and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata exposes the trigger through
  `INFORMATION_SCHEMA.TRIGGERS`, keeps the `.TRG` and `.TRN` files present,
  and preserves the trigger body in `SHOW CREATE TRIGGER`.
- Firing the trigger before creating the referenced audit table returns
  MariaDB `1146` and leaves the base table unchanged.
- Creating the audit table lets the recovered trigger fire and persist base and
  audit rows.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic invalid-dependency trigger crash coverage, not full
  trigger crash fuzzing.
- Explicit current-user definer crash recovery is covered by
  `ownerless-trigger-definer-crash`; stored-function trigger crash recovery is
  covered by `ownerless-trigger-stored-function-crash`, while broader
  privilege/security and randomized trigger crash variants remain planned.
- Full external MariaDB/RQG long-running DDL stress remains planned.
