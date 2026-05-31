# Ownerless Table-Lock Waiter Death

## Problem Statement

Ownerless table-lock wait state is stored in the directory-backed InnoDB lock
registry, but current crash coverage focuses on SQL record-lock waits. Before
claiming broader table-lock wait hardening, the primitive must prove that a
process killed while waiting for a table lock leaves an explicit waiting entry
that remains observable until owner cleanup removes it.

This slice adds that bounded primitive coverage. It does not claim a reliable
SQL `LOCK TABLES` fault path; source inspection and exploratory selector runs
showed explicit SQL table-lock shapes can be intercepted by MDL or page-write
gates before reaching the intended InnoDB table-wait callback. Ownerless
`LOCK TABLES` is now an explicit unsupported policy surface until SQL
locked-table mode is designed.

## Source Findings

MariaDB base line:

- `mariadb-11.8.6`
- source ref `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`

Relevant source paths:

- `mariadb/storage/innobase/lock/lock0lock.cc`
  - `lock_table_low()` consults MyLite's table reservation hook before local
    grant and creates a local waiting table lock when the shared registry
    reports an external conflict.
  - `mylite_ownerless_innodb_lock_wait_for_external_grant()` snapshots the
    waiting table-lock key and waits through
    `mylite_ownerless_innodb_lock_wait_for_external()`.
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  - `mylite_ownerless_innodb_lock_wait_for_external()` dispatches table waits
    to the MyLite `wait_until_table` callback.
- `packages/libmylite/src/ownerless_innodb_lock_registry.cc`
  - `mylite_ownerless_innodb_lock_registry_wait_until_table_available()` uses
    the same wait-entry machinery as record waits: it publishes a waiting entry
    with blocker owner/transaction IDs before sleeping on the blocker wait
    word.
  - `mylite_ownerless_innodb_lock_registry_release_owner()` removes an owner's
    granted and waiting entries and wakes affected peers.
- `packages/libmylite/tests/ownerless_primitives_test.c`
  - Existing primitive coverage proves table compatibility and record wait
    cleanup, but it does not kill a process while a table wait entry is live.

## Design

Add a primitive cross-process test:

1. Initialize an InnoDB lock-registry mapping.
2. Owner 1 acquires an `X` table lock.
3. A child process using owner 2 waits for an incompatible `S` table lock with
   a long timeout.
4. The parent polls until the shared waiting count reaches one.
5. The parent kills the child and verifies the waiting entry remains present.
6. Owner cleanup for owner 2 removes the dead waiter's waiting entry.
7. Releasing owner 1 removes the granted table lock and leaves the registry
   empty.

## Scope

In scope:

- Ownerless InnoDB lock-registry primitive coverage for table wait entries
  across waiter process death.
- Spec and compatibility updates that narrow the remaining table-lock SQL fault
  gap instead of hiding it.

Out of scope:

- Adding a new MariaDB hook point.
- Claiming deterministic SQL-level table-lock fault injection.
- Changing lock-registry layout, wait ordering, deadlock policy, or cleanup
  semantics.

## Compatibility Impact

No public C API behavior changes. This is platform evidence for the ownerless
InnoDB table wait state that SQL table-lock integration depends on. Ownerless
SQL `LOCK TABLES` is deliberately rejected, while SQL-level table-lock fault
injection for native table-wait paths remains explicitly planned.

## Directory And Lifecycle Impact

No directory layout changes. The test uses a standalone mapped registry file
with the same volatile wait-entry layout used inside
`concurrency/mylite-concurrency.shm`.

## Native Storage Impact

No native storage files are opened. The test covers directory-owned table wait
coordination, not InnoDB data-page or redo behavior.

## Binary Size Impact

Test-only coverage. No production dependency, public symbol, or durable state
is added.

## Test Plan

- Build and run the ownerless primitives test target.
- Run the ownerless hook CTest filter covering primitives, cross-process SQL,
  and negative proof tests.
- Rebuild and run the normal embedded ownerless SQL filter.
- Run `format-check` and `git diff --check`.

## Acceptance Criteria

- A table waiter publishes exactly one shared waiting entry before sleeping.
- Killing the waiter does not silently erase that entry.
- Owner cleanup removes the dead waiter's waiting entry.
- Releasing the blocker leaves no active or waiting table entries.
- Existing ownerless primitive, hook, and embedded SQL coverage remains green.

## Risks And Open Questions

- This is primitive fault coverage. SQL-level table-lock fault injection still
  needs a reliable MariaDB execution shape that reaches the table-wait callback
  before MDL, page-write gates, or row-lock waits intercept it. Ownerless
  `LOCK TABLES` support separately needs a design for MariaDB locked-table
  mode and `UNLOCK TABLES` cleanup.
