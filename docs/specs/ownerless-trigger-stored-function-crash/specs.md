# Ownerless Trigger Stored-Function Crash

## Problem Statement

Ownerless trigger crash coverage already proves simple create/drop,
replacement, ordering, idempotent no-op, delayed invalid-dependency, and
explicit-definer trigger metadata survives a writer death after MariaDB native
trigger file changes but before MyLite publishes ownerless dictionary finish.
The remaining bounded trigger-body edge is a trigger definition that calls a
stored function.

Ownerless mode still rejects stored routine execution. This slice does not
enable routine support; it proves MyLite recovers the native trigger metadata
at the crash boundary and still fails closed when the recovered trigger tries
to execute its stored-function body in ownerless mode.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB stored-function documentation describes stored functions as named
  routines callable from SQL expressions. MyLite already treats that execution
  path as unsupported in ownerless mode.
- `mariadb/sql/sql_yacc.yy` parses trigger bodies as stored-program statements,
  including expressions that can call stored functions.
- `mariadb/sql/sql_trigger.cc:657` through
  `mariadb/sql/sql_trigger.cc:667` dispatches trigger create/drop through
  `Table_triggers_list::create_trigger()` or
  `Table_triggers_list::drop_trigger()`.
- `mariadb/sql/sql_trigger.cc:926` through
  `mariadb/sql/sql_trigger.cc:1143` writes the trigger-name `.TRN` file and
  table-level `.TRG` file for accepted trigger definitions.
- `mariadb/sql/sp_head.cc:sp_head::execute_function()` is the stored-function
  execution entry point guarded by MyLite's ownerless routine-execution policy.
- `packages/libmylite/src/database.cc` starts ownerless dictionary DDL for
  ownerless `CREATE TRIGGER`, and the unsafe `dictionary-before-finish` hook
  can kill the writer after MariaDB native SQL execution but before ownerless
  dictionary finish publication.

## Design

Add a hook-only selector,
`dictionary-trigger-stored-function-crash`, to
`mylite_ownerless_cross_process_sql_test`:

1. Create an InnoDB base table and a stored function with an ordinary exclusive
   open, because ownerless stored-routine DDL remains rejected.
2. Keep a live ownerless peer open.
3. Kill a writer after ownerless `CREATE TRIGGER` writes native `.TRG`/`.TRN`
   metadata for a `BEFORE INSERT` trigger whose body calls the stored function,
   but before ownerless dictionary finish.
4. Prove the live peer prevents dead-writer cleanup until no-live ownerless
   recovery.
5. Verify the recovered trigger and routine metadata through
   `INFORMATION_SCHEMA`, native trigger files, and `SHOW CREATE TRIGGER`.
6. Verify firing the recovered trigger in ownerless mode fails with the existing
   stored-routine execution diagnostic and leaves the base table unchanged.
7. Verify ordinary exclusive native reopen can still execute the stored
   function through the recovered trigger, then remove the temporary row so
   repeated ownerless/native reopen checks observe the same empty base table.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for a trigger body that calls a
  stored function,
- recovered `.TRG` and `.TRN` files under `datadir/app/`,
- `INFORMATION_SCHEMA.TRIGGERS`, `INFORMATION_SCHEMA.ROUTINES`, and
  `SHOW CREATE TRIGGER` verification,
- ownerless routine-execution rejection when the recovered trigger fires,
- ordinary native successful trigger firing after recovery,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- enabling ownerless stored routine execution,
- ownerless stored-routine DDL support,
- privilege/security variants beyond the existing current-user definer crash
  coverage,
- randomized trigger crash oracle coverage,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No new ownerless routine or trigger behavior is enabled. The slice strengthens
ownerless trigger crash evidence while preserving the current compatibility
boundary: ownerless stored routine execution remains unsupported, including
trigger-body stored-function calls.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The selector exercises native
MariaDB trigger metadata files under the MyLite-owned database directory:

- `datadir/app/ownerless_trigger_function_crash_base.TRG`,
- `datadir/app/ownerless_trigger_function_crash_bi.TRN`.

The stored function remains MariaDB routine metadata inside `datadir/mysql/`
because it is created before ownerless mode is entered.

## Native Storage Impact

Trigger and stored-function metadata are MariaDB SQL-layer metadata. The base
table is InnoDB, so the selector also verifies post-recovery DML and native
tablespace persistence, but it does not change InnoDB file formats,
page-version replay, redo/checkpoint policy, or file lifecycle routing.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run the focused selector:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-trigger-stored-function-crash`
- Run adjacent trigger/routine selectors:
  - `dictionary-trigger-invalid-dependency-crash`
  - `dictionary-trigger-definer-crash`
  - `routine-execution-policy`
- Run the hook crash-tail selector, focused ownerless CTest labels,
  `format-check`, and `git diff --check`.

## Acceptance Criteria

- The focused selector reaches the dictionary fault hook and does not hang.
- A live peer prevents cleanup until no-live recovery.
- Recovered metadata exposes the trigger and stored function through
  `INFORMATION_SCHEMA`, keeps the `.TRG` and `.TRN` files present, and
  preserves the stored-function call in `SHOW CREATE TRIGGER`.
- Ownerless firing fails with the stored-routine execution diagnostic and
  leaves the base table unchanged.
- Ordinary native firing executes the stored function through the recovered
  trigger.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic stored-function trigger crash coverage, not full
  trigger crash fuzzing.
- Broader trigger privilege/security variants and randomized trigger crash
  oracles remain planned.
- Full external MariaDB/RQG long-running DDL stress remains planned.
