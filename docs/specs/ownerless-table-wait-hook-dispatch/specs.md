# Ownerless Table-Wait Hook Dispatch

## Problem Statement

Ownerless table-wait correctness has primitive, native SQL reachability, native
SQL waiter-death, and SQL negative-proof coverage, but the embedded hook test did
not directly assert the C dispatcher used by retained external table-wait
snapshots. A regression in `mylite_ownerless_innodb_lock_wait_for_external()`
could therefore break field propagation or result handling while higher-level
SQL tests still diagnose the issue indirectly.

This slice adds direct hook-dispatch coverage. It does not expand the product
claim to SQL-level table-lock fault injection.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/innobase/lock/mylite_ownerless_innodb_lock_hooks.cc`
  implements `mylite_ownerless_innodb_lock_wait_for_external()`. For
  `MYLITE_OWNERLESS_INNODB_LOCK_EXTERNAL_WAIT_TABLE`, it loads the installed
  `wait_until_table` callback and forwards the stable transaction ID, InnoDB
  table ID, normalized table-lock mode, timeout, and hook context.
- `mariadb/storage/innobase/include/mylite_ownerless_innodb_lock_hooks.h`
  exposes `struct mylite_ownerless_innodb_lock_external_wait`, whose table-wait
  variant is independent of native `lock_t` layout and can be tested without
  constructing upstream InnoDB internals.
- Existing ownerless SQL coverage already proves the external native table-wait
  registry path for the reachable `LOCK IN SHARE MODE` reader plus
  `foreign_key_checks=0` and `unique_checks=0` bulk-insert writer shape, and
  separately proves explored representative DDL shapes do not reach the local
  ownerless table-wait callback.

## Design

Extend `embedded_ownerless_innodb_lock_hooks_test` so its installed
`wait_until_table` hook records the dispatched fields and returns a configurable
result. The new test:

1. Verifies table external waits are unavailable before hooks are installed.
2. Verifies `NONE` snapshots return success and null snapshots return error.
3. Installs hooks, dispatches a table snapshot, and asserts exact transaction,
   table, mode, and timeout propagation.
4. Verifies callback timeout results propagate to the caller.
5. Verifies invalid snapshot kinds are rejected while hooks are installed.

## Scope

In scope:

- Embedded hook-dispatch test coverage.
- Documentation that narrows the table-wait evidence boundary.

Out of scope:

- New MariaDB lock hook points.
- SQL locked-table mode support.
- Broader SQL-level table-lock fault injection.
- Directory layout, lock-registry, wait/deadlock, or product capability changes.

## Compatibility Impact

No public SQL or C API behavior changes. The added coverage supports existing
ownerless table-wait compatibility claims by testing an internal hook boundary.
Ownerless `LOCK TABLES` and `UNLOCK TABLES` remain unsupported in ownerless
read/write mode.

## Directory And Lifecycle Impact

No durable or volatile directory layout changes. The test runs entirely through
the embedded hook test and resets hooks before returning.

## Native Storage Impact

No native storage files are opened. The slice only covers dispatcher behavior for
a stable external wait snapshot.

## Binary Size Impact

No production code changes. Test-only C code grows slightly.

## Test Plan

- Build and run `libmylite.embedded-ownerless-innodb-lock-hooks` under the
  production preset.
- Run neighboring table-wait hook coverage under `ownerless-test-hooks`:
  `libmylite.ownerless-native-table-wait`,
  `libmylite.ownerless-native-table-wait-crash`,
  `libmylite.ownerless-table-wait-negative-proof`, and
  `libmylite.ownerless-primitives`.
- Run production-build guard, formatting, and whitespace checks.

## Acceptance Criteria

- External table-wait snapshots dispatch through `wait_until_table`.
- Stable transaction ID, table ID, mode, and timeout fields are forwarded
  unchanged.
- Timeout and invalid-kind results are observable at the dispatcher boundary.
- Existing native table-wait SQL and primitive coverage still pass.

## Risks And Open Questions

- This is direct hook coverage, not a SQL-level table-lock fault-injection
  breakthrough. Positive SQL-level local table-wait fault injection remains
  unclaimed unless a MariaDB execution shape reaches the local table-wait
  callback in a deterministic and product-relevant way.
