# MTR routed storage foreign-key smoke

## Problem

MyLite has first-party routed-storage foreign-key coverage, but the
storage-routed MTR suite does not yet prove that raw embedded MTR execution can
create and enforce representative `ENGINE=InnoDB` and explicit
`ENGINE=MYLITE` foreign keys through the MyLite handler. This leaves the MTR
storage runner focused on basic engine routing and transaction behavior rather
than relational integrity.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::mylite_init_func()` advertises
  `HTON_SUPPORTS_FOREIGN_KEYS` for the MyLite handlerton.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_supported_engine_request()`
  accepts `InnoDB` and explicit `MYLITE` as requested engines routed to MyLite
  storage.
- `mariadb/storage/mylite/ha_mylite.cc` validates supported FK definitions in
  `mylite_validate_foreign_key_definitions()` during routed table creation and
  copy ALTER.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::write_row()` and
  `ha_mylite::delete_row()` enforce supported child and parent checks during
  row DML.
- MariaDB documents foreign keys as constraints that require child values to
  reference parent values and prevent deleting referenced parent rows under
  restrictive actions:
  <https://mariadb.com/kb/en/foreign-keys/>.

## Design

Add `mylite.routed_storage_foreign_keys` to the storage MTR list. The test
creates `ENGINE=InnoDB` and explicit `ENGINE=MYLITE` parent/child table pairs
while MyLite is the enforced storage engine, checks `SHOW CREATE TABLE` FK
publication, verifies child orphan insert rejection, verifies parent delete
rejection while referenced, then drops the FK and confirms each parent can be
deleted without native sidecars.

## Scope

This is test and documentation work only. It covers the raw embedded MTR path
for one supported `RESTRICT` / `NO ACTION`-style foreign key. It does not add
new FK actions, broader multi-table DML matrices, cyclic FKs, recursive
actions, or full InnoDB parity.

## Compatibility Impact

The new test backs the compatibility claim that application DDL using
`ENGINE=InnoDB` or explicit `ENGINE=MYLITE` can route to MyLite storage while
retaining representative foreign-key metadata and enforcement.

## Storage And Lifecycle Impact

Durable parent, child, index, and FK metadata stay inside the primary
`.mylite` file. The test reuses the existing sidecar assertion to reject
native `.frm`, `.ibd`, MyISAM, Aria, binlog, and relay-log files.

## Verification Plan

- `tools/mylite-mtr-harness run-storage mylite.routed_storage_foreign_keys`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- The new storage MTR FK test passes.
- The full storage-routed MTR list passes.
- Compatibility docs mention routed FK coverage in storage MTR mode.

## Verification Results

- `tools/mylite-mtr-harness probe-storage mylite.routed_storage_foreign_keys`
  passed after adding explicit `ENGINE=MYLITE` FK coverage.
