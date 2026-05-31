# Ownerless Trigger Ordering And Show Create

## Problem Statement

Ownerless trigger coverage now proves simple trigger create/fire/drop plus
replacement and UPDATE/DELETE trigger execution. Broader trigger edge cases
remain partial, especially the MariaDB paths that store multiple trigger
definitions for one base table, maintain `ACTION_ORDER`, and resolve
`SHOW CREATE TRIGGER` by trigger name before the table metadata lock is known.

This slice adds bounded ownerless SQL evidence for multi-trigger ordering and
trigger-name lookup from an already-open peer. It does not claim full trigger
compatibility, trigger definer/security behavior, invalid dependencies, stored
functions called by triggers, or crash recovery during trigger DDL.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/sql/sql_yacc.yy`
  - The trigger grammar accepts optional `FOLLOWS` or `PRECEDES` clauses after
    `FOR EACH ROW` and records them in `LEX::trg_chistics`.
- `mariadb/sql/sql_trigger.cc`
  - `build_trig_stmt_query()` stores trigger definitions without
    `FOLLOWS`/`PRECEDES`, so execution order must come from
    `Trigger::action_order` and the in-memory trigger list.
  - `Table_triggers_list::create_trigger()` verifies the ordering anchor
    trigger exists with the same event and timing before adding the new
    trigger.
  - `Table_triggers_list::add_trigger()` inserts a trigger before or after the
    anchor and recomputes `action_order` for following triggers.
  - `Table_triggers_list::find_trigger()` searches all event/timing trigger
    lists by trigger name and is used by both drop and show-create paths.
  - `add_table_for_trigger_internal()` reads the trigger-name `.TRN` file to
    discover the base table for `DROP TRIGGER`.
- `mariadb/sql/sql_show.cc`
  - `get_schema_triggers_record()` exposes `ACTION_ORDER` in
    `INFORMATION_SCHEMA.TRIGGERS`.
  - `show_create_trigger()` calls `get_trigger_table()`, which reads the
    trigger-name `.TRN` file through `load_table_name_for_trigger()` before
    opening the base table and finding the trigger in `Table_triggers_list`.
  - `show_create_trigger_impl()` returns the trigger name and original SQL
    statement to the client.
- `mariadb/sql/sql_base.cc`
  - MariaDB documents that trigger metadata is protected by the base table
    metadata lock, but `SHOW CREATE TRIGGER` and `DROP TRIGGER` first perform a
    dictionary lookup by trigger name because their syntax does not name the
    base table.
- `packages/libmylite/src/database.cc`
  - MyLite ownerless dictionary DDL classification treats `CREATE` and `DROP`
    statements as ownerless dictionary-generation boundaries. `SHOW CREATE
    TRIGGER` is a read path and must observe the generated trigger metadata
    after peer refresh.

## Design

Add a focused selector, `trigger-ordering`, to
`mylite_ownerless_cross_process_sql_test`.

The selector starts an ownerless parent handle and a child ownerless DDL
process:

1. The child creates InnoDB base/audit tables, creates two `AFTER INSERT`
   triggers, and declares the second trigger `FOLLOWS` the first.
2. The parent observes both trigger-name `.TRN` files plus the table-level
   `.TRG` file, checks `INFORMATION_SCHEMA.TRIGGERS.ACTION_ORDER`, runs
   `SHOW CREATE TRIGGER` by trigger name, inserts a base row, and verifies the
   audit table records firing order.
3. The child creates a third `AFTER INSERT` trigger `PRECEDES` the first
   trigger. The parent verifies refreshed action-order metadata, `SHOW CREATE
   TRIGGER`, and firing order from the already-open handle.
4. The child drops all three triggers. The parent verifies metadata/file
   absence and proves later inserts no longer fire trigger bodies.
5. Final assertions verify base/audit rows plus trigger absence through
   ownerless and ordinary exclusive reopen before and after forced `.shm`
   rebuild.

## Scope

In scope:

- SQL-level ownerless coverage for `FOLLOWS` and `PRECEDES` trigger ordering.
- Already-open peer refresh for multiple trigger definitions stored in one
  table-level `.TRG` file and three trigger-name `.TRN` files.
- `SHOW CREATE TRIGGER` by trigger name from an already-open ownerless peer.
- Final trigger absence and base/audit durability through ownerless/native
  reopen before and after volatile `.shm` rebuild.
- Compatibility and cross-process-concurrency documentation updates.

Out of scope:

- New production dictionary or storage code unless the selector reveals a bug.
- Definer/security semantics, invalid trigger dependencies, `SHOW TRIGGERS`,
  stored functions called by triggers, or prepared statement metadata for
  trigger show paths.
- Crash/fault injection during trigger DDL.

## Compatibility Impact

No intended SQL behavior change. The slice expands ownerless compatibility
evidence for trigger ordering and `SHOW CREATE TRIGGER` lookup while keeping
broader trigger semantics partial. Ordinary exclusive embedded behavior
continues to inherit MariaDB behavior.

## Directory And Lifecycle Impact

No directory layout changes. The selector exercises native trigger metadata
inside the MyLite-owned database directory:

- `datadir/app/ownerless_trigger_order_base.TRG` while triggers exist,
- `datadir/app/ownerless_trigger_order_first.TRN`,
- `datadir/app/ownerless_trigger_order_second.TRN`,
- `datadir/app/ownerless_trigger_order_third.TRN`.

Final checks verify those files are absent after `DROP TRIGGER` and that the
InnoDB base/audit tables survive ownerless/native reopen before and after
volatile shared-memory rebuild.

## Native Storage Impact

The base and audit tables are InnoDB. The selector exercises native INSERT
paths and trigger-maintained audit rows through ownerless metadata refresh, but
it does not change InnoDB file formats, redo/page-version replay semantics, or
page ownership rules.

## Binary Size Impact

Test and documentation only unless a bug fix is required. No dependency,
public API, or default runtime feature is added.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run the focused `trigger-ordering` selector in `embedded-dev`.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run the relevant ownerless SQL CTest filters after implementation.
- Run `format-check`, `git diff --check`, and cached diff checks before
  commit.

## Acceptance Criteria

- An already-open ownerless peer sees two ordered triggers created by another
  process and verifies `ACTION_ORDER`.
- The same peer can execute `SHOW CREATE TRIGGER` by trigger name for a peer
  created trigger.
- The same peer observes a third `PRECEDES` trigger and refreshed
  `ACTION_ORDER` without closing its handle.
- Trigger firing order matches `FOLLOWS`/`PRECEDES` metadata.
- The same peer sees all triggers disappear after `DROP TRIGGER`, and later
  base-table DML does not execute dropped trigger bodies.
- Final trigger absence and base/audit table state survive ownerless/native
  reopen before and after forced `.shm` rebuild.
- Docs continue to mark untested trigger edge cases and crash recovery as
  planned.

## Risks And Open Questions

- MariaDB stores trigger definitions without the original ordering clause and
  relies on trigger-list order. This slice proves bounded refresh for one
  ordering graph, not all multi-trigger reorder or invalid-anchor cases.
- Trigger security/definer behavior, invalid dependencies, trigger bodies that
  call stored functions, and randomized trigger oracles remain planned.
