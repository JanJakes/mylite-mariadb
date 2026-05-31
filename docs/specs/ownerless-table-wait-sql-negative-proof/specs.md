# Ownerless Table-Wait SQL Negative Proof

## Problem Statement

Ownerless table-lock wait fault injection remains a planned gap, but the
currently explored SQL shapes do not prove a reachable path to the native
InnoDB table-wait callback. A blocked ownerless `ALTER TABLE` behind an active
transaction times out with MariaDB lock-wait behavior, yet source inspection
shows that path can be intercepted before
`mylite_ownerless_innodb_lock_publish_table_wait()` publishes a table wait.

This slice adds hook-only evidence for that boundary. It creates an unsafe test
fault marker at the existing MyLite table-wait callback and adds a focused SQL
negative-proof selector that fails if the representative blocked `ALTER TABLE`
ever reaches that callback. The slice does not claim deterministic SQL-level
table-lock crash injection.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/storage/innobase/lock/lock0lock.cc`
  - `lock_table_low()` first checks for a local incompatible table lock through
    `lock_table_other_has_incompatible()`. Only that local-queue case calls
    `lock_table_enqueue_waiting()`.
  - `lock_table_enqueue_waiting()` creates a local waiting table lock and calls
    `mylite_ownerless_innodb_lock_publish_table_wait(lock, c_lock)`, which is
    the callback boundary this slice instruments.
  - When no local incompatible lock exists,
    `lock_table_low()` asks the ownerless shared registry to reserve the table
    lock before grant. A shared external conflict maps to
    `mylite_ownerless_innodb_lock_enqueue_external_table_wait()`, which creates
    a local waiting table lock without publishing a table-wait callback at that
    point.
  - `mylite_ownerless_innodb_lock_try_grant_external_wait()` can later publish
    a table wait if the local waiting lock also has to wait behind a local
    table lock while retrying the external wait.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  - `mylite_ownerless_innodb_lock_publish_table_wait()` calls the MyLite
    wait-table callback only for publishable table waits whose wait and blocker
    locks have nonzero transaction IDs.
- `packages/libmylite/src/database.cc`
  - `ownerless_innodb_lock_wait_table_hook()` records a table wait in the
    directory-backed InnoDB lock registry.
  - Existing unsafe test fault coverage pauses record external waits and
    record-lock grants, but not this table-wait callback boundary.
- `packages/libmylite/tests/ownerless_cross_process_sql_test.c`
  - `test_ownerless_alter_waits_for_active_transaction()` proves a blocked
    ownerless `ALTER TABLE ... ADD COLUMN` behind `SELECT ... FOR UPDATE`
    returns MariaDB error 1205, then succeeds after the holder releases.
  - Existing ownerless policy coverage rejects SQL `LOCK TABLES` and
    `UNLOCK TABLES`, so locked-table mode is not a supported route to force a
    table-wait fault.

## Design

Add an unsafe-test-only pause point named `table-lock-wait` inside
`ownerless_innodb_lock_wait_table_hook()`. Normal builds and normal hook builds
without the environment variable remain unchanged.

Add a hook-build SQL selector, `table-lock-wait-negative-proof`, that:

1. Creates the ordinary ownerless InnoDB fixture.
2. Starts an ownerless transaction that holds
   `SELECT value FROM app.ownerless_sql WHERE id = 1 FOR UPDATE`.
3. Starts a second ownerless process with the `table-lock-wait` fault
   configured and runs the representative blocked
   `ALTER TABLE app.ownerless_sql ADD COLUMN note VARCHAR(32)` with
   `lock_wait_timeout = 1`.
4. Waits for either the child to exit with the expected MariaDB 1205 timeout or
   the fault-ready pipe to become readable.
5. Treats fault readiness as a failure, because that means the SQL shape now
   reaches the native table-wait callback and the negative-proof boundary is
   stale.
6. Releases the holder and verifies the table remains usable by successfully
   applying the same `ALTER TABLE`, updating the new column, and reopening via
   ownerless and native handles.

## Scope

In scope:

- Test-hook-only instrumentation of the existing MyLite table-wait callback.
- A bounded SQL negative proof for the currently representative blocked
  `ALTER TABLE` shape.
- Documentation that narrows the remaining table-lock fault gap.

Out of scope:

- New MariaDB hook points.
- Supporting ownerless SQL locked-table mode.
- Claiming SQL-level table-lock crash recovery or table-wait fault injection.
- Changing lock-registry layout, conflict rules, or wait/deadlock policy.

## Compatibility Impact

No public SQL or C API behavior changes. Ownerless `ALTER TABLE` timeout
behavior remains covered by the existing SQL test. The new selector is
hook-build evidence that the current SQL timeout shape does not exercise the
native table-wait callback. Ownerless SQL `LOCK TABLES` and `UNLOCK TABLES`
remain deliberately unsupported.

## Directory And Lifecycle Impact

No directory layout changes. The selector uses normal ownerless database
directories and verifies cleanup through the ordinary open/close lifecycle.

## Native Storage Impact

The representative `ALTER TABLE` that times out must leave the original InnoDB
table unchanged. After the holder releases, the same DDL is applied normally and
verified through ownerless and native reopen.

## Binary Size Impact

The only production-source change is inside
`MYLITE_ENABLE_UNSAFE_OWNERLESS_TEST_HOOKS`. No public symbol, dependency,
durable state, or default embedded behavior is added.

## Test Plan

- Build the normal embedded ownerless SQL target to verify non-hook builds.
- Build the `ownerless-test-hooks` ownerless SQL target.
- Run
  `./build/ownerless-test-hooks/packages/libmylite/mylite_ownerless_cross_process_sql_test table-lock-wait-negative-proof`.
- Run the ownerless hook CTest filter for cross-process SQL.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- The representative blocked `ALTER TABLE` returns MariaDB errno 1205 while
  the holder transaction is active.
- The `table-lock-wait` fault-ready pipe remains unreadable for that SQL shape.
- If a future source change makes the SQL shape reach the table-wait callback,
  this selector fails loudly instead of silently preserving stale evidence.
- Existing ownerless SQL and hook coverage remains green.

## Risks And Open Questions

- This is negative evidence, not feature completion. A future slice still needs
  a reliable SQL execution shape or a narrower source hook if SQL-level
  table-lock crash injection becomes reachable and worth supporting.
- SQL locked-table mode remains unsupported until MariaDB's connection-level
  table/handler lock lifecycle and `UNLOCK TABLES` cleanup have an ownerless
  design.
