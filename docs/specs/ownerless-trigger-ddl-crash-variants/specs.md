# Ownerless Trigger DDL Crash Variants

## Problem Statement

Simple ownerless trigger crash coverage proves completed `CREATE TRIGGER` and
`DROP TRIGGER` metadata survive a writer death after MariaDB native trigger
file changes but before MyLite publishes ownerless dictionary finish. The next
trigger gap is variant metadata that rewrites existing trigger files or depends
on MariaDB trigger action ordering.

This slice adds deterministic crash-boundary evidence for:

- `CREATE OR REPLACE TRIGGER` replacing an existing `BEFORE UPDATE` trigger,
- `CREATE TRIGGER ... PRECEDES ...` inserting a trigger before existing
  same-event/same-timing triggers and recomputing `ACTION_ORDER`.

## Source Findings

Base: MariaDB 11.8 LTS import `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_trigger.cc:657` through
  `mariadb/sql/sql_trigger.cc:663` dispatches trigger create/drop to
  `Table_triggers_list::create_trigger()` or
  `Table_triggers_list::drop_trigger()`.
- `mariadb/sql/sql_trigger.cc:926` through
  `mariadb/sql/sql_trigger.cc:1143` implements trigger creation: it validates
  `FOLLOWS`/`PRECEDES` anchors, backs up `.TRG`/`.TRN` files when replacement
  or existing table triggers require it, drops the old trigger for
  `OR REPLACE`, creates the trigger-name `.TRN`, inserts the trigger in the
  table trigger list, and writes the table-level `.TRG`.
- `mariadb/sql/sql_trigger.cc:796` through
  `mariadb/sql/sql_trigger.cc:866` builds the stored trigger definition and
  deliberately omits `FOLLOWS`/`PRECEDES` from the stored trigger text.
- `mariadb/sql/sql_trigger.cc:2014` through
  `mariadb/sql/sql_trigger.cc:2047` inserts a trigger before or after the
  anchor trigger and recomputes `action_order`.
- `mariadb/sql/sql_yacc.yy:18511` through
  `mariadb/sql/sql_yacc.yy:18525` parses `FOLLOWS` and `PRECEDES` trigger
  ordering clauses.
- `packages/libmylite/src/database.cc:9304` runs the unsafe
  `dictionary-before-finish` hook after native SQL execution but before
  ownerless dictionary finish.

## Design

Add two unsafe-hook selectors to
`mylite_ownerless_cross_process_sql_test`:

- `dictionary-trigger-replace-crash` creates an InnoDB base table, inserts one
  row, creates a `BEFORE UPDATE` trigger that increments `NEW.value` by one,
  keeps a live ownerless peer open, kills a writer after
  `CREATE OR REPLACE TRIGGER` rewrites the native trigger metadata to increment
  by two, verifies live-peer cleanup remains busy, then reopens no-live
  ownerless/native and verifies the replacement trigger body is recovered and
  fires.
- `dictionary-trigger-order-crash` creates two ordered `AFTER INSERT` triggers,
  keeps a live ownerless peer open, kills a writer after a third trigger is
  created with `PRECEDES ownerless_trigger_order_crash_first`, verifies
  live-peer cleanup remains busy, then reopens no-live ownerless/native and
  verifies `.TRG`/`.TRN` file presence, `INFORMATION_SCHEMA.TRIGGERS`
  `ACTION_ORDER`, `SHOW CREATE TRIGGER`, and firing order.

Both selectors verify ownerless and ordinary native reopen before and after
forced `.shm` rebuild.

## Scope And Non-Goals

In scope:

- crash-at-dictionary-before-finish coverage for trigger replacement and
  ordered trigger insertion,
- recovered present `INFORMATION_SCHEMA.TRIGGERS` metadata,
- recovered `.TRG` and `.TRN` files under `datadir/app/`,
- recovered replacement trigger body behavior,
- recovered ordered trigger `ACTION_ORDER` and firing order,
- ownerless/native reopen before and after forced `.shm` rebuild.

Out of scope:

- idempotent trigger crash no-op paths,
- trigger definer/security semantics,
- invalid dependency handling,
- trigger bodies that call stored functions,
- randomized trigger oracle coverage,
- SQL-level table-lock fault injection for native table-wait paths.

## Compatibility Impact

No SQL feature is newly enabled. The slice strengthens existing ownerless
trigger compatibility evidence by proving completed replacement and ordered
trigger native metadata survives a writer death at MyLite's dictionary
publication boundary.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The tests exercise MariaDB native
trigger metadata files under the MyLite-owned `datadir/app/` directory,
ownerless process cleanup, dictionary-generation recovery, forced `.shm`
rebuild, and ordinary native exclusive reopen lifecycle.

## Native Storage Impact

Trigger metadata is MariaDB-native SQL-layer metadata. The base and audit
tables are InnoDB, so the selectors also check base-table durability and
post-recovery trigger-maintained rows, but they do not alter InnoDB storage
formats or page-version replay policy.

## Public API, Build, Size, And Dependencies

No public API, build-profile, binary-size, license, or dependency changes.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` with
  `ownerless-test-hooks`.
- Run focused selectors:
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-trigger-replace-crash`
  - `build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test dictionary-trigger-order-crash`
- Run the normal embedded `trigger-ddl`, `trigger-variants`,
  `trigger-ordering`, and `trigger-idempotent-ddl` selectors.
- Run the hook crash-tail selector and ownerless hook SQL shards.
- Run `format-check`, `dev` CTest, `tidy`, and `git diff --check`.

## Acceptance Criteria

- The focused selectors reach the dictionary fault hook and do not hang.
- A live peer prevents cleanup until no-live recovery.
- Replacement recovery exposes the trigger through
  `INFORMATION_SCHEMA.TRIGGERS`, preserves the `.TRG` and `.TRN` files,
  preserves the replacement body in `SHOW CREATE TRIGGER`, and fires the
  replacement body through base-table updates.
- Ordering recovery exposes three triggers through
  `INFORMATION_SCHEMA.TRIGGERS`, preserves the `.TRG` and three `.TRN` files,
  reports the recovered `PRECEDES` trigger as `ACTION_ORDER = 1`, and fires the
  triggers in the recovered order.
- Ownerless and ordinary native reopen observe the same state before and after
  forced `.shm` rebuild.

## Risks And Unresolved Questions

- This is deterministic trigger replacement and ordering crash coverage, not
  the full trigger crash matrix.
- Idempotent no-op trigger DDL, security/definer, invalid dependency,
  stored-function, and randomized trigger crash variants remain separate
  candidate slices.
- Full external MariaDB/RQG long-running DDL stress remains planned.
