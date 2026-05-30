# Ownerless Trigger DDL Refresh

## Problem

Ownerless DDL coverage now covers table, schema, and simple view metadata
refresh. Triggers remain grouped with broader routine metadata gaps even though
MariaDB stores trigger definitions in database-directory files tied to the base
table and opens them through the table-trigger cache. Ownerless peers need
evidence that `CREATE TRIGGER` and `DROP TRIGGER` publish through the
dictionary-generation protocol, refresh already-open peers, and leave the base
table durable after no-live reopen.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documentation for `CREATE TRIGGER`
  (`https://mariadb.com/docs/server/server-usage/triggers-events/triggers/create-trigger`)
  describes trigger creation on a table for `INSERT`, `UPDATE`, or `DELETE`
  events with `BEFORE` or `AFTER` timing.
- MariaDB documentation for `DROP TRIGGER`
  (`https://mariadb.com/docs/server/reference/sql-statements/data-definition/drop/drop-trigger`)
  describes trigger deletion by schema-qualified trigger name.
- `mariadb/sql/sql_parse.cc` dispatches `SQLCOM_CREATE_TRIGGER` and
  `SQLCOM_DROP_TRIGGER` to `mysql_create_or_drop_trigger()`.
- `mariadb/sql/sql_trigger.cc:mysql_create_or_drop_trigger()` takes an
  exclusive trigger metadata lock, opens the subject base table, waits while the
  table is used, then routes creation or deletion through the table's
  `Table_triggers_list`.
- `mariadb/sql/sql_base.cc` documents that ordinary trigger DDL and DML are
  protected by the metadata lock on the table that owns the trigger; only
  `SHOW CREATE TRIGGER` and `DROP TRIGGER` need a dictionary lookup by trigger
  name before acquiring the table lock.
- `mariadb/sql/sql_trigger.cc:Table_triggers_list::create_trigger()` writes a
  table-level `.TRG` trigger definition file and a trigger-name `.TRN` file
  using `build_table_filename()` plus `sql_create_definition_file()`.
- `mariadb/sql/sql_trigger.cc:Table_triggers_list::drop_trigger()` removes the
  trigger from the in-memory trigger list, rewrites or removes the table-level
  `.TRG` file, and removes the trigger-name `.TRN` file.
- `mariadb/sql/sql_show.cc:get_schema_triggers_record()` reads table trigger
  metadata for `INFORMATION_SCHEMA.TRIGGERS`.
- MyLite ownerless DDL classification in `packages/libmylite/src/database.cc`
  treats `CREATE` and `DROP` statements as dictionary DDL, so trigger creation
  and deletion publish through the same ownerless odd/even
  dictionary-generation protocol used by table, schema, and view DDL.

## Scope And Non-Goals

- Add a focused ownerless SQL selector for trigger create/fire/drop metadata.
- Verify an already-open ownerless peer observes a trigger created by another
  ownerless process, inserts into the base table, and sees the trigger write to
  an InnoDB audit table.
- Verify the same peer observes `DROP TRIGGER`, and later base-table inserts no
  longer fire the dropped trigger.
- Verify final base/audit table state and removed `.TRG`/`.TRN` files survive
  ownerless/native reopen before and after forced `.shm` rebuild.
- Do not add stored-function, multi-trigger ordering, `SHOW CREATE TRIGGER`,
  prepared routine, or trigger crash-recovery coverage.
- Do not add SQL-level table-lock fault injection; prior exploratory SQL shapes
  did not reach the ownerless table-wait callback.

## Design

- Add `trigger-ddl` to `mylite_ownerless_cross_process_sql_test`.
- The child ownerless process creates InnoDB base and audit tables, creates an
  `AFTER INSERT` trigger on the base table, inserts the first base row, and
  signals the parent.
- The parent keeps an ownerless handle open, observes the trigger through
  `information_schema.triggers`, verifies the `.TRG` and `.TRN` files exist,
  inserts a second base row, and checks the trigger-maintained audit total.
- The child drops the trigger. The parent observes trigger metadata absence
  from the same already-open handle, inserts a third base row, and verifies the
  audit total does not change after the drop.
- After both ownerless handles close, helper assertions verify the final base
  and audit totals plus removed trigger definition files through:
  - `MYLITE_OPEN_READWRITE | MYLITE_OPEN_OWNERLESS_RW`,
  - `MYLITE_OPEN_READWRITE`,
  - forced `concurrency/mylite-concurrency.shm` deletion plus ownerless reopen,
  - ordinary exclusive read/write reopen after the forced rebuild.

## Compatibility Impact

No SQL behavior changes. The compatibility matrix gains ownerless evidence for
simple trigger create/fire/drop dictionary refresh and no-live reopen while
keeping broader triggers, stored functions, routines, and trigger edge cases
partial.

## Directory And Lifecycle Impact

The slice exercises MariaDB trigger metadata inside the MyLite-owned database
directory: `datadir/app/ownerless_trigger_base.TRG` and
`datadir/app/ownerless_trigger_ai.TRN` while the trigger exists, and absence of
both files after `DROP TRIGGER` and no-live reopen. It also verifies the InnoDB
base and audit tables remain durable through ownerless and ordinary exclusive
reopen before and after volatile shared-memory recreation.

## Native Storage Impact

No native storage format changes. The trigger body writes to an InnoDB audit
table, so the test exercises ownerless DML visibility and trigger metadata
refresh together while table rows remain in MariaDB native InnoDB files.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `trigger-ddl` selector.
- Build and run the focused `trigger-ddl` selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage or a focused rerun if the
  known intermittent InnoDB log-header checksum abort appears.
- Run the ownerless stress preset, `format-check`, `git diff --check`, and
  cached diff checks before commit.

## Acceptance Criteria

- Already-open ownerless peers see a trigger created by another ownerless
  process.
- The peer can fire the trigger through base-table DML and observe the audit
  table effect.
- Already-open peers see the trigger disappear after `DROP TRIGGER`, and later
  base-table DML does not execute the dropped trigger.
- Final trigger absence and base/audit table state survive ownerless/native
  reopen before and after forced `.shm` rebuild.
- Compatibility docs keep broader trigger/routine and external-oracle stress
  gaps marked partial/planned.

## Risks And Follow-Up

- Multiple triggers per event/timing, trigger ordering, `SHOW CREATE TRIGGER`,
  definer/security edge cases, invalid dependency handling, stored functions,
  and prepared routine calls remain outside this slice.
- Crash recovery during trigger DDL and broader DDL-created file lifecycle
  recovery remain planned.
