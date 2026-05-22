# MTR routed storage transaction smoke

## Problem

The storage-routed MTR suite proves basic engine alias routing and sidecar
absence, but it does not yet exercise MariaDB transaction statements through
the raw embedded MTR path. That leaves a gap between the first-party CTest
transaction coverage and the opt-in MTR storage runner.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB documents `START TRANSACTION`, `COMMIT`, and `ROLLBACK` as the
  transaction boundary statements, with `ROLLBACK` canceling current
  transaction changes and `COMMIT` making them permanent:
  <https://mariadb.com/kb/en/start-transaction/>.
- MariaDB documents `SAVEPOINT`, `ROLLBACK TO SAVEPOINT`, and
  `RELEASE SAVEPOINT` as partial-transaction controls:
  <https://mariadb.com/docs/server/reference/sql-statements/transactions/savepoint>.
- `mariadb/sql/transaction.cc` implements these SQL statements through
  `trans_begin()`, `trans_commit()`, `trans_rollback()`,
  `trans_savepoint()`, `trans_rollback_to_savepoint()`, and
  `trans_release_savepoint()`.
- `mariadb/sql/handler.cc` documents that storage engines must register with
  `trans_register_ha()` so the server can commit or roll back statement and
  normal transactions.
- `mariadb/storage/mylite/ha_mylite.cc` registers MyLite handlerton
  transaction callbacks in `mylite_init_func()` and registers the handler with
  MariaDB transactions in `ha_mylite::external_lock()`.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_supported_engine_request()`
  accepts `InnoDB`, `MyISAM`, `Aria`, `BLACKHOLE`, `MEMORY`, and `HEAP` as
  MyLite-routed requested-engine names.

## Design

Add a MyLite-owned storage MTR case, `mylite.routed_storage_transactions`,
that runs only through `tools/mylite-mtr-harness run-storage`. The test creates
an explicit `ENGINE=InnoDB` table while `default_storage_engine` and
`enforce_storage_engine` are `MYLITE`, then verifies:

- `SHOW CREATE TABLE` preserves the requested `InnoDB` engine name.
- `ROLLBACK` undoes inserted and updated rows.
- `COMMIT` persists inserted and updated rows.
- `SAVEPOINT`, `ROLLBACK TO SAVEPOINT`, and `RELEASE SAVEPOINT` keep changes
  before the savepoint and undo later changes.
- The existing MyLite sidecar assertion reports no native durable schema or
  engine sidecars.

## Scope

This is test and documentation work only. It does not change storage behavior,
transaction implementation, file format, public API, or build configuration
outside the curated storage MTR list.

## Compatibility Impact

The test adds MTR-path evidence for transactional MyLite storage when a
MySQL/MariaDB application requests `ENGINE=InnoDB`. It does not claim full
InnoDB parity, isolation-level coverage, cross-process concurrency, XA, or
broader savepoint matrices.

## Storage And Lifecycle Impact

Durable application state remains in the primary `.mylite` file. The test
allows only documented MyLite-owned transaction and rollback journals and
checks that native `.frm`, `.ibd`, MyISAM, Aria, binlog, and relay-log sidecars
are absent.

## Verification Plan

- `tools/mylite-mtr-harness run-storage mylite.routed_storage_transactions`
- `tools/mylite-mtr-harness list-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- The new MTR test passes through the storage MTR harness.
- The storage MTR list includes `mylite.routed_storage_transactions`.
- Compatibility docs mention storage-routed MTR transaction/savepoint coverage.
- No unrelated upstream MariaDB tests are added after failed probes.

## Risks

The test is intentionally narrow. It proves the raw embedded MTR path for one
representative routed `InnoDB` table, not the full transaction roadmap.
