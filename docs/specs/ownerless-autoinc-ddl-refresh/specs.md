# Ownerless AUTO_INCREMENT DDL Refresh

## Problem

Ownerless write concurrency already uses a directory-backed AUTO_INCREMENT
high-watermark registry so concurrent implicit inserts from separate embedded
processes do not reuse IDs. A related DDL/space-allocation boundary remains:
`ALTER TABLE ... AUTO_INCREMENT = N` updates native InnoDB metadata and the
clustered-index root page, while already-open ownerless peers may still hold
old table and AUTO_INCREMENT state.

The ownerless dictionary-generation refresh path must prove that an already-open
peer observes the altered high watermark before its next implicit insert, and
that lowering the AUTO_INCREMENT option does not regress below the rows already
present or the shared registry high watermark.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/handler/ha_innodb.cc` implements
  `ha_innobase::innobase_get_autoinc()` and
  `ha_innobase::get_auto_increment()`. MyLite's ownerless read hook can raise
  the native table AUTO_INCREMENT value from the shared registry, and the
  publish hook records the next reserved value after implicit insert
  reservation.
- `mariadb/storage/innobase/handler/handler0alter.cc:commit_set_autoinc()`
  persists user-supplied `ALTER TABLE ... AUTO_INCREMENT` values, and for lower
  values scans the existing maximum row value so InnoDB does not reuse IDs.
- `packages/libmylite/src/ownerless_autoinc_registry.cc` stores one monotonic
  high-watermark slot per InnoDB table id and uses `max(stored, seed)` when a
  process reads or seeds a table's next value.
- `packages/libmylite/src/database.cc` registers the ownerless AUTO_INCREMENT
  hooks and refreshes ownerless peers across dictionary-generation changes
  before later statements run.

## Scope And Non-Goals

- Add focused ownerless SQL coverage for `ALTER TABLE ... AUTO_INCREMENT` from
  one process while another process already has the table open.
- Verify a peer implicit insert uses an increased DDL high watermark.
- Verify a later lower AUTO_INCREMENT option does not reuse IDs.
- Verify the final state through ownerless reopen, native exclusive reopen, and
  forced `.shm` rebuild.
- Do not change AUTO_INCREMENT registry layout, table id lookup, SQL syntax
  support, or native InnoDB AUTO_INCREMENT semantics unless the focused test
  exposes a product bug.

## Design

- Add an `auto-inc-ddl` ownerless SQL selector.
- The selector creates an InnoDB table with one implicit AUTO_INCREMENT row,
  then starts a child ownerless handle that waits for parent signals.
- The already-open parent ownerless handle stays live while the child executes:
  - `ALTER TABLE app.ownerless_auto_inc_ddl AUTO_INCREMENT = 50`
  - `ALTER TABLE app.ownerless_auto_inc_ddl AUTO_INCREMENT = 2`
- After the increase, the parent inserts an implicit row and verifies it gets
  id `50`.
- After the lower setting, the parent inserts another implicit row and verifies
  it gets id `51`.
- After both handles close, the test verifies the rows and high watermark
  through ownerless and ordinary read/write reopen before and after deleting
  `concurrency/mylite-concurrency.shm`, then inserts once more after the forced
  rebuild and verifies id `52`.

## Compatibility Impact

No new SQL surface is added. The slice strengthens the existing ownerless
partial claim for InnoDB AUTO_INCREMENT by covering DDL high-watermark refresh
in addition to concurrent implicit insert allocation.

## Directory And Lifecycle Impact

No new files or layout changes. The test uses existing InnoDB table files,
the ownerless shared-memory registry, and forced `.shm` recreation inside the
MyLite database directory.

## Native Storage Impact

The slice relies on MariaDB's native InnoDB AUTO_INCREMENT persistence on the
clustered-index root page and MyLite's ownerless registry as a cross-process
monotonic guard. It does not alter the native file format.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `auto-inc` and `auto-inc-ddl` selectors.
- Build and run the same focused selectors in `ownerless-test-hooks`.
- Run the embedded ownerless SQL CTest selector, hook ownerless SQL label,
  ownerless stress preset, `format-check`, and diff checks.

## Acceptance Criteria

- An already-open ownerless peer uses id `50` after another ownerless process
  raises `AUTO_INCREMENT` to `50`.
- Lowering the option to `2` does not reuse IDs; the already-open peer uses
  id `51`.
- After forced `.shm` rebuild, a later ownerless implicit insert uses id `52`.
- Existing ownerless AUTO_INCREMENT, DDL, hook, and stress coverage remains
  green.

## Risks And Follow-Up

- This covers single-table ownerless DDL high-watermark refresh. Adding a new
  AUTO_INCREMENT column during table rebuild is covered separately by
  `ownerless-autoinc-column-ddl-refresh`.
- External MariaDB/RQG stress remains a separate planned oracle for broader
  DDL and allocation behavior.
