# Ownerless Trigger DDL Variants

## Problem Statement

Ownerless trigger coverage currently proves a simple `AFTER INSERT`
`CREATE TRIGGER`, peer-fired audit effect, `DROP TRIGGER`, and no-live reopen.
The compatibility matrix still keeps broader trigger edge cases planned. That
leaves a gap for trigger definition replacement and non-insert trigger
timing/event paths that rely on MariaDB's table-trigger cache and OLD/NEW row
accessors.

This slice adds bounded SQL evidence for trigger replacement plus UPDATE and
DELETE trigger execution through an already-open ownerless peer. It does not
claim full trigger compatibility, multi-trigger ordering, security/definer
semantics, stored functions, or crash recovery during trigger DDL.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - The `create_or_replace ... TRIGGER` grammar accepts
    `CREATE OR REPLACE TRIGGER`.
  - Trigger grammar records `BEFORE`/`AFTER`, `INSERT`/`UPDATE`/`DELETE`, and
    optional `FOLLOWS`/`PRECEDES` ordering in `LEX::trg_chistics`.
- `mariadb/sql/sql_parse.cc`
  - `SQLCOM_CREATE_TRIGGER` and `SQLCOM_DROP_TRIGGER` dispatch to
    `mysql_create_or_drop_trigger()`.
- `mariadb/sql/sql_trigger.cc`
  - `mysql_create_or_drop_trigger()` takes an exclusive trigger metadata lock,
    opens and locks the subject base table, waits while the table is used, and
    routes creation/deletion through the table's `Table_triggers_list`.
  - `Table_triggers_list::create_trigger()` backs up existing trigger files
    when needed, drops the old trigger for `OR REPLACE`, writes the
    trigger-name `.TRN` file, adds the trigger to the in-memory trigger list,
    and writes the table-level `.TRG` definition file.
  - `Table_triggers_list::drop_trigger()` removes the trigger from the
    in-memory list, rewrites or removes the table-level `.TRG` file, and
    removes the trigger-name `.TRN` file.
  - `Table_triggers_list::prepare_record_accessors()` sets up `NEW` fields for
    `BEFORE INSERT`/`BEFORE UPDATE` triggers and OLD-row fields for
    UPDATE/DELETE triggers.
  - `Table_triggers_list::process_triggers()` executes the selected
    event/timing trigger list and applies `OLD`/`NEW` row bindings.
- `mariadb/sql/sql_update.cc` and `mariadb/sql/sql_delete.cc`
  - UPDATE and DELETE statement paths call `process_triggers()` for relevant
    `TRG_EVENT_UPDATE` and `TRG_EVENT_DELETE` timing classes.
- `packages/libmylite/src/database.cc`
  - MyLite ownerless dictionary DDL classification treats `CREATE`, `ALTER`,
    and `DROP` statements as ownerless dictionary-generation boundaries, so
    trigger replacement and deletion publish through the same odd/even
    generation protocol as other covered DDL.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - The existing `trigger-ddl` selector covers only simple `AFTER INSERT`
    create/fire/drop metadata refresh.

## Design

Add a focused selector, `trigger-ddl-variants`, to
`mylite_ownerless_cross_process_sql_test`.

The selector starts an ownerless parent handle and a child ownerless DDL
process:

1. The child creates InnoDB base and audit tables, inserts one base row, and
   creates a `BEFORE UPDATE` trigger that increments `NEW.value` by one.
2. The parent observes the trigger through `INFORMATION_SCHEMA.TRIGGERS` and
   the `.TRG`/`.TRN` files, updates the base row from the already-open handle,
   and verifies the stored value reflects the `BEFORE UPDATE` trigger.
3. The child runs `CREATE OR REPLACE TRIGGER` for the same trigger name with a
   different increment. The parent updates the row again and verifies the new
   trigger body is visible without closing the handle.
4. The child creates an `AFTER DELETE` trigger that writes OLD row values into
   the InnoDB audit table. The parent deletes the base row and verifies the
   audit effect.
5. The child drops both triggers. The parent observes metadata/file absence and
   proves later UPDATE/DELETE operations no longer fire trigger bodies.
6. Final assertions verify base/audit rows plus trigger absence through
   ownerless and ordinary exclusive reopen before and after forced `.shm`
   rebuild.

## Scope

In scope:

- SQL-level ownerless coverage for `CREATE OR REPLACE TRIGGER`.
- Already-open peer dictionary/table-trigger cache refresh for a replaced
  trigger definition.
- `BEFORE UPDATE` `NEW` value mutation and `AFTER DELETE` `OLD` value audit
  effects through a peer ownerless handle.
- Trigger `.TRG` and `.TRN` file presence/absence inside the MyLite database
  directory.
- Compatibility and cross-process-concurrency documentation updates.

Out of scope:

- New production dictionary or storage code unless the selector reveals a bug.
- Multiple triggers for the same event/timing, `FOLLOWS`/`PRECEDES` ordering,
  `SHOW CREATE TRIGGER`, definer/security behavior, invalid dependency
  handling, or stored functions called by triggers.
- Crash/fault injection during trigger DDL.
- SQL-level table-lock fault injection.

## Compatibility Impact

No intended SQL behavior change. The slice expands ownerless compatibility
evidence for trigger DDL variants while keeping broader trigger semantics
partial. Ordinary exclusive embedded behavior continues to inherit MariaDB
behavior.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises MariaDB trigger metadata
inside the MyLite-owned database directory:

- `datadir/app/ownerless_trigger_variant_base.TRG` while triggers exist,
- `datadir/app/ownerless_trigger_variant_bu.TRN` while the UPDATE trigger
  exists,
- `datadir/app/ownerless_trigger_variant_ad.TRN` while the DELETE trigger
  exists.

Final checks verify those files are absent after `DROP TRIGGER` and that the
InnoDB base/audit tables survive ownerless/native reopen before and after
volatile shared-memory rebuild.

## Native Storage Impact

The base and audit tables are InnoDB. The selector exercises native table
UPDATE and DELETE paths through trigger metadata refresh, but it does not
change InnoDB file formats, redo/page-version replay semantics, or page
ownership rules.

## Binary Size Impact

Test and documentation only unless a bug fix is required. No dependency,
public API, or default runtime feature is added.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `trigger-ddl-variants` selector in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest filters after implementation.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- An already-open ownerless peer sees a trigger created by another process.
- The same peer fires the original `BEFORE UPDATE` trigger and observes the
  changed row value.
- The same peer sees the `CREATE OR REPLACE TRIGGER` definition change without
  closing its handle.
- The same peer fires an `AFTER DELETE` trigger that reads OLD row values.
- The same peer sees both triggers disappear after `DROP TRIGGER`, and later
  base-table DML does not execute the dropped triggers.
- Final trigger absence and base/audit table state survive ownerless/native
  reopen before and after forced `.shm` rebuild.
- Docs continue to mark untested trigger edge cases and crash recovery as
  planned.

## Risks And Open Questions

- MariaDB trigger replacement rewrites native `.TRG` and `.TRN` metadata files.
  This slice proves dictionary-generation refresh for bounded replacement
  shapes, not crash recovery if a process dies mid-rewrite.
- Multi-trigger ordering, trigger security/definer behavior, trigger bodies
  that call stored functions, invalid dependencies, and randomized trigger
  oracles remain planned.
