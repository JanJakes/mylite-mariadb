# Ownerless Trigger Idempotent Crash

## Problem Statement

Ownerless trigger crash coverage proves completed trigger create, drop,
replacement, and ordering metadata survives a writer death after MariaDB native
trigger work but before MyLite publishes ownerless dictionary finish. Normal
ownerless trigger idempotent coverage proves `CREATE TRIGGER IF NOT EXISTS` and
`DROP TRIGGER IF EXISTS` no-op semantics through already-open peers.

The remaining bounded gap is the crash boundary around the no-op paths
themselves. A process can still own the MyLite dictionary-generation section
while MariaDB has intentionally left the trigger files unchanged. A later
no-live opener must clean the dead owner and preserve the original trigger
state.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy:18578` through
  `mariadb/sql/sql_yacc.yy:18585` parses `CREATE TRIGGER` with
  `opt_if_not_exists` before the trigger name.
- `mariadb/sql/sql_yacc.yy:13502` through
  `mariadb/sql/sql_yacc.yy:13506` parses `DROP TRIGGER` with
  `opt_if_exists` before the trigger name.
- `mariadb/sql/sql_parse.cc:5784` through
  `mariadb/sql/sql_parse.cc:5798` dispatches both create and drop trigger
  statements through `mysql_create_or_drop_trigger()`.
- `mariadb/sql/sql_trigger.cc:658` through
  `mariadb/sql/sql_trigger.cc:667` calls
  `Table_triggers_list::create_trigger()` for create and
  `Table_triggers_list::drop_trigger()` for drop.
- `mariadb/sql/sql_trigger.cc:1033` through
  `mariadb/sql/sql_trigger.cc:1072` handles an existing trigger-name `.TRN`
  file: `OR REPLACE` drops/replaces, `IF NOT EXISTS` emits
  `ER_TRG_ALREADY_EXISTS` as a note and returns success without replacing the
  trigger body, and a plain duplicate create returns an error.
- `mariadb/sql/sql_trigger.cc:2125` through
  `mariadb/sql/sql_trigger.cc:2135` resolves a missing trigger-name `.TRN` file
  for `DROP TRIGGER`; when `IF EXISTS` is present, MariaDB emits
  `ER_TRG_DOES_NOT_EXIST` as a note, leaves the target table unset, and returns
  success before table trigger files are mutated.
- `packages/libmylite/src/database.cc:9254` through
  `packages/libmylite/src/database.cc:9296` starts the ownerless dictionary DDL
  section for ownerless `CREATE` and `DROP` statements before SQL execution.
- `packages/libmylite/src/database.cc:9299` through
  `packages/libmylite/src/database.cc:9332` runs the unsafe
  `dictionary-before-finish` hook after SQL execution but before dictionary
  generation finish.

## Design

Add two unsafe-hook selectors to
`mylite_ownerless_cross_process_sql_test`:

- `dictionary-trigger-idempotent-create-crash` creates an InnoDB base/audit
  pair and an initial `AFTER INSERT` trigger, then kills a writer after a
  duplicate `CREATE TRIGGER IF NOT EXISTS` statement with a different body
  reaches `dictionary-before-finish`.
- `dictionary-trigger-idempotent-drop-crash` creates an InnoDB base/audit pair
  and an initial `AFTER INSERT` trigger, then kills a writer after
  `DROP TRIGGER IF EXISTS` for a missing trigger reaches
  `dictionary-before-finish`.

Both selectors keep a live ownerless peer open while the writer is killed, prove
cleanup remains busy until no-live recovery, then verify ownerless and ordinary
native reopen before and after forced `.shm` rebuild.

The follow-up in
`docs/specs/ownerless-trigger-idempotent-replace-live-recovery/specs.md`
promotes these idempotent/no-op trigger selectors to metadata-only live-peer
recovery.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for duplicate
  `CREATE TRIGGER IF NOT EXISTS` no-op behavior,
- crash-at-dictionary-before-finish coverage for missing
  `DROP TRIGGER IF EXISTS` no-op behavior,
- original trigger `.TRG` and `.TRN` preservation,
- absence of the missing trigger-name `.TRN` file after no-op drop recovery,
- `INFORMATION_SCHEMA.TRIGGERS` and `SHOW CREATE TRIGGER` verification,
- trigger firing through the original body after recovery,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- broader privilege/security semantics,
- trigger bodies that call stored functions,
- randomized trigger crash oracle coverage,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL behavior is newly enabled. The slice strengthens ownerless evidence for
MariaDB-compatible idempotent trigger DDL by proving a dead writer in MyLite's
dictionary publication window does not turn native no-op statements into
replacement, removal, or stale peer state.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The tests exercise native MariaDB
trigger metadata files under the MyLite-owned `datadir/app/` directory:

- `ownerless_trigger_idempotent_create_crash_base.TRG`,
- `ownerless_trigger_idempotent_create_crash_ai.TRN`,
- `ownerless_trigger_idempotent_drop_crash_base.TRG`,
- `ownerless_trigger_idempotent_drop_crash_ai.TRN`.

The missing drop selector also proves the missing trigger-name `.TRN` file is
not created during recovery.

## Native Storage Impact

Trigger metadata is MariaDB-native SQL-layer metadata. The base and audit
tables are InnoDB so the selectors also verify post-recovery DML, but they do
not change InnoDB file formats, page-version replay, redo/checkpoint policy, or
file lifecycle routing.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-trigger-idempotent-create-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-trigger-idempotent-drop-crash`
- Run the normal embedded `trigger-idempotent-ddl` selector.
- Run adjacent hook trigger crash selectors and `crash-tail`.
- Run ownerless SQL CTest filters, `format-check`, `tidy`, and
  `git diff --check`.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- Duplicate idempotent create recovery preserves the original trigger body in
  `SHOW CREATE TRIGGER`, keeps the `.TRG` and `.TRN` files present, and fires
  the original body rather than the duplicate statement body.
- Missing idempotent drop recovery preserves the real trigger, keeps the real
  `.TRG` and `.TRN` files present, keeps the missing `.TRN` file absent, and
  fires the real trigger body.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic no-op trigger DDL crash coverage, not full trigger
  crash fuzzing.
- Delayed invalid-dependency trigger crash recovery is covered by
  `ownerless-trigger-invalid-dependency-crash`; explicit current-user definer
  crash recovery is covered by `ownerless-trigger-definer-crash`, while broader
  privilege/security and randomized trigger crash variants
  remain planned.
- Metadata-only live-peer recovery for these idempotent/no-op trigger forms is
  covered by
  `docs/specs/ownerless-trigger-idempotent-replace-live-recovery/specs.md`.
- Full external MariaDB/RQG long-running DDL stress remains planned.
