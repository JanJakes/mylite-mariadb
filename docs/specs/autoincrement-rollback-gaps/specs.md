# Autoincrement Rollback Gaps

## Goal

Preserve generated `AUTO_INCREMENT` gaps when a durable MyLite-routed table
rolls back a transaction or savepoint. MariaDB/InnoDB treats persistent
autoincrement counters as non-transactional, so row rollback must remove the
row while keeping the consumed generated value unavailable for later inserts.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- MariaDB's `AUTO_INCREMENT` documentation says InnoDB values reserved by a
  transaction are lost when that transaction fails, such as by `ROLLBACK`.
- MariaDB's InnoDB autoincrement documentation says persistent autoincrement
  does not mean transactional and specifically names `ROLLBACK` and
  `ROLLBACK TO SAVEPOINT` as cases where gaps can occur:
  <https://mariadb.com/docs/server/server-usage/storage-engines/innodb/auto_increment-handling-in-innodb>.
- `mariadb/sql/handler.cc:handler::update_auto_increment()` fills the current
  row value from `get_auto_increment()`, remembers reserved intervals, and
  advances `next_insert_id` for following rows.
- `mariadb/storage/innobase/handler/ha_innodb.cc:ha_innobase::get_auto_increment()`
  updates InnoDB's table autoincrement state when it reserves an interval in
  the non-old-style lock modes.
- `mariadb/storage/innobase/handler/ha_innodb.cc:innobase_savepoint()` records
  an undo number for row rollback; the autoincrement counter is outside the row
  undo state.
- `packages/mylite-storage/src/storage.c:mylite_storage_rollback_statement()`
  currently restores the checkpoint catalog and header pages. That correctly
  hides rolled-back row, row-state, and index pages, but it also hides
  autoincrement state pages appended after the checkpoint.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` advances durable
  MyLite autoincrement state before publishing the row payload and index
  entries, so the storage layer can preserve generated counters without
  keeping rolled-back rows.

## Scope

- Preserve advancing autoincrement state pages appended during rollback of:
  - a durable transaction checkpoint; and
  - a nested statement checkpoint, which is the direct `libmylite` savepoint
    model.
- Preserve only table IDs that already existed in the checkpoint catalog.
- Preserve only values greater than the checkpoint-visible next value.
- Cover direct SQL transaction rollback, direct savepoint rollback, prepared
  inserts inside a transaction, close/reopen persistence, and first-party
  storage checkpoint behavior.

## Non-Goals

- Preserve autoincrement gaps for top-level failed autocommit statements whose
  only rollback mechanism is a statement checkpoint.
- Change DDL rollback semantics. Failed table DDL should continue to restore
  catalog and table state from the checkpoint.
- Add handler-level savepoint hooks for MariaDB's native savepoint API.
- Add new lock modes, cross-process allocation guarantees, or WAL.
- Change grouped-prefix allocation; grouped-prefix counters remain derived from
  live rows rather than durable table-local autoincrement pages.

## Compatibility Impact

This moves MyLite closer to InnoDB-style autoincrement persistence for routed
durable tables. Rolled-back generated rows no longer let the next insert reuse
the generated value after `ROLLBACK` or direct `ROLLBACK TO SAVEPOINT`.

Top-level failed-statement gaps remain partial. That is deliberate because
failed DDL rollback also uses statement checkpoints, and the storage layer must
not make failed metadata changes durable by accident.

## Design

Before restoring checkpoint pages, rollback scans pages appended after the
checkpoint header's `page_count` up to the current header's `page_count`.
Autoincrement pages are collected when their table ID exists in the checkpoint
catalog. If multiple pages exist for the same table, the latest page in file
order wins.

Rollback then restores the checkpoint catalog and header pages as it does now.
After restoration, it republishes the collected autoincrement values whose
`next_value` is greater than the checkpoint-visible value for the same table.
Republishing uses the existing single-page autoincrement publication path and
therefore stores the preserved counter inside the primary `.mylite` file.

The rollback path performs this preservation only when the checkpoint is a
durable transaction checkpoint or has a parent checkpoint. Plain top-level
statement rollback keeps the previous all-state restore behavior.

## File Lifecycle

No file-format change is required. The preserved state is another ordinary
autoincrement page appended to the primary `.mylite` file after rollback
restores the header and catalog. Existing recovery journals continue to protect
header publication. If a process dies before rollback completes, the existing
transaction journal can still restore the pre-rollback checkpoint state.

## Embedded Lifecycle And API

No public `libmylite` API change is required. The behavior is visible through
ordinary direct and prepared SQL execution over a file-backed database.

## Storage-Engine Routing

The behavior applies to durable MyLite-routed tables. A requested
`ENGINE=InnoDB` table is physically routed to MyLite and therefore receives
this MyLite implementation of InnoDB-compatible autoincrement gaps.
Volatile `MEMORY`/`HEAP` rows are out of scope because their autoincrement
state is process-local and not published through durable storage pages.

## Build, Size, And Dependencies

No dependency, license, public ABI, or intended size-profile change is
introduced. The implementation is first-party storage code plus tests and docs.

## Test Plan

- Add storage tests proving transaction rollback preserves an advancing
  autoincrement page while rolling back row state.
- Add storage tests proving nested checkpoint rollback preserves a higher
  autoincrement page, but a lower `SET AUTO_INCREMENT` page is not republished
  over the checkpoint-visible value.
- Add SQL storage-engine coverage for direct transaction rollback, direct
  savepoint rollback, prepared inserts inside a transaction, and close/reopen.
- Run focused storage and storage-engine tests, the storage-smoke preset, the
  compatibility transaction and statement-rollback groups, shell syntax checks,
  and `git diff --check`.

## Acceptance Criteria

- A generated value consumed inside `ROLLBACK` is not reused by the next insert
  into the same durable routed table.
- A generated value consumed before `ROLLBACK TO SAVEPOINT` is not reused after
  the savepoint rollback.
- Prepared inserts inside a rolled-back transaction preserve the same gap.
- Rows and index entries created after the checkpoint are still rolled back.
- Failed DDL rollback coverage remains green.
- Compatibility and roadmap docs distinguish this transaction/savepoint
  coverage from broader failed-statement autoincrement gaps.

## Risks And Open Questions

- Top-level failed statement gaps remain unresolved. Covering them likely needs
  handler-level reservation tracking that can distinguish DML-generated
  autoincrement pages from DDL metadata changes.
- Crash recovery during rollback can restore the checkpoint state before the
  preserved autoincrement page is finalized. The acceptance target is completed
  rollback persistence, not crash-in-the-middle rollback gap preservation.
