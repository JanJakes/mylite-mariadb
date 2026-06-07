# Ownerless Event Scheduler Policy

## Problem

MariaDB events are scheduled database objects run by the server event
scheduler. MyLite's core embedded contract has no daemon-owned scheduler, and
ownerless read/write mode is designed around coordinated foreground SQL
statements over a MyLite-owned database directory. Event DDL and scheduler
variables would introduce server-owned `mysql.event` metadata and background
execution semantics that are not part of the ownerless concurrency model.

The general MyLite server-surface policy already rejects event SQL, but
ownerless coverage should prove that the policy remains in force while
ownerless runtime hooks are installed and that rejected event statements leave
no event metadata or table side effects.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documentation describes events as named database objects run
  according to a schedule by the event scheduler.
- `mariadb/sql/sql_yacc.yy` parses `CREATE EVENT`, `ALTER EVENT`, `DROP
  EVENT`, `SHOW EVENTS`, and `SHOW CREATE EVENT` into event command families.
- `mariadb/sql/events.cc` implements the server event scheduler and event
  metadata paths around `mysql.event`.
- MyLite `packages/libmylite/src/database.cc` rejects event DDL and event
  metadata statements in `is_unsupported_account_or_event_statement()`.
- MyLite `packages/libmylite/src/database.cc` rejects `event_scheduler` in
  server variable assignment policy.
- Existing embedded server-surface coverage tests ordinary direct and prepared
  event policy rejection; ownerless cross-process SQL coverage did not have a
  focused event/scheduler selector.

## Scope And Non-Goals

- Add a focused ownerless `event-policy` selector to
  `mylite_ownerless_cross_process_sql_test`.
- Cover direct ownerless rejection for `CREATE EVENT`, `CREATE DEFINER ...
  EVENT`, `CREATE OR REPLACE EVENT`, `ALTER EVENT`, `ALTER DEFINER ... EVENT`,
  `DROP EVENT`, `SHOW EVENTS`, `SHOW CREATE EVENT`, and `SET` of the global
  event scheduler variable.
- Cover prepared ownerless rejection for representative event DDL and metadata
  inspection.
- Verify a normal InnoDB table remains writable after rejected event SQL and
  that rejected event names are absent through ownerless/native reopen before
  and after forced shared-memory rebuild.
- Do not add event support, scheduler support, `mysql.event` ownerless
  coordination, background worker execution, or external RQG stress.
- Do not revisit SQL-level table-lock fault injection; prior ownerless SQL
  shapes did not reach the ownerless table-wait callback.

## Design

No runtime policy change is needed. This slice reuses the existing MyLite
server-surface gate, which runs before direct execution and prepared statement
preparation. The ownerless test opens a database with
`MYLITE_OPEN_OWNERLESS_RW`, creates an ordinary InnoDB table, exercises direct
and prepared rejected event/scheduler SQL, then inserts another row to prove
the handle remains usable after the policy diagnostics.

The final assertions reopen the database in ownerless and ordinary read/write
mode, force deletion of `concurrency/mylite-concurrency.shm`, and repeat both
reopens. Each reopen verifies the table rows and confirms the rejected event
names are absent from `information_schema.events`.

## Compatibility Impact

Events and the event scheduler remain out of scope for the core embedded
profile and for ownerless read/write mode. This slice turns that boundary into
ownerless-specific evidence while preserving existing ordinary event policy
coverage.

Applications that require scheduled server-side jobs need a higher-level
integration layer or an application-owned scheduler around `libmylite`; the
core library does not run background SQL jobs.

## Directory And Lifecycle Impact

No directory layout changes are introduced. The policy prevents ownerless event
DDL from creating `mysql.event` rows or enabling scheduler-owned state, and the
test verifies rejected event names remain absent across ownerless/native reopen
and forced shared-memory rebuild.

## Native Storage Impact

No native storage format changes. Supported ordinary InnoDB table DML remains
usable after event/scheduler policy diagnostics.

## Binary Size And Dependencies

No dependency or license changes. The only binary impact is focused test code;
runtime policy already existed.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `event-policy` in `embedded-dev`.
- Build and run focused `event-policy` in `ownerless-test-hooks`.
- Run adjacent ownerless policy selectors and embedded ownerless cross-process
  SQL coverage as practical.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- Ownerless event DDL, event metadata inspection, and event scheduler variable
  assignments fail with the existing MyLite server-surface policy diagnostic.
- Prepared ownerless event DDL and metadata inspection fail before statement
  allocation.
- Rejected event SQL leaves no matching `information_schema.events` metadata.
- The ordinary InnoDB table remains writable and survives ownerless/native
  reopen before and after forced `.shm` rebuild.
- Compatibility docs continue to mark events and scheduler out of scope while
  identifying ownerless policy coverage.

## Risks And Follow-Up

- Future event support would need a separate design for scheduler ownership,
  background execution, `mysql.event` metadata coordination, crash recovery,
  and interaction with ownerless process lifetimes.
- Broader ownerless DDL/file lifecycle recovery and external MariaDB/RQG stress
  remain planned ownerless-concurrency gaps.
