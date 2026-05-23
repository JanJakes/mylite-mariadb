# MTR routed storage foreign-key action smoke

## Problem

The storage-routed MTR suite covers basic foreign-key publication and
`RESTRICT` / `NO ACTION` enforcement, while first-party storage smoke covers
supported `SET NULL` and `CASCADE` action paths. The raw embedded MTR storage
runner does not yet prove those action paths for applications that request
`ENGINE=InnoDB` but are routed to MyLite, or for explicit `ENGINE=MYLITE`
tables.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `ON UPDATE` / `ON DELETE` clauses and maps
  `CASCADE`, `SET NULL`, `NO ACTION`, `RESTRICT`, and `SET DEFAULT` to
  `enum_fk_option`.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_foreign_key_storage_action()`
  maps MariaDB FK options to MyLite storage metadata action values.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_foreign_key_actions_supported()`
  admits the current supported action subset after checking child-table shape.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_append_foreign_key_clause()`
  renders persisted action metadata back into `SHOW CREATE TABLE`.
- `packages/mylite-storage/src/storage.c::validate_foreign_key_action()` accepts
  persisted `CASCADE`, `SET NULL`, `NO ACTION`, `RESTRICT`, `SET DEFAULT`, and
  unspecified action values.

## Design

Add `mylite.routed_storage_foreign_key_actions` to the storage MTR list. The
test runs with a primary `.mylite` file, enforced MyLite storage, and sidecar
assertions. It creates matching `ENGINE=InnoDB` and `ENGINE=MYLITE` table pairs
for:

- `ON DELETE SET NULL ON UPDATE SET NULL` over nullable child columns;
- `ON DELETE CASCADE ON UPDATE CASCADE` over non-null child columns;
- `SHOW CREATE TABLE` action metadata; and
- forced-index reads after `ON UPDATE CASCADE`.

## Scope

This is compatibility test and documentation work only. It does not add new FK
support, expand FK shape admission, implement cyclic action graphs, or broaden
transaction-aware FK behavior.

## Compatibility Impact

The storage-routed MTR runner gains raw embedded evidence for supported
foreign-key action behavior under both `ENGINE=InnoDB` alias routing and
explicit `ENGINE=MYLITE`. The broader FK compatibility claim remains partial:
cyclic or full recursive FK action graphs and exhaustive multi-table
update/delete matrices remain planned.

## Storage And Lifecycle Impact

All FK metadata and row changes remain in the primary `.mylite` file. The test
keeps the existing storage MTR sidecar assertion, so native schema directories
or native engine table files fail the slice.

## Public API, File-Format, And Size Impact

No public `libmylite` API, file-format, production dependency, or binary-size
change. This adds only MTR coverage and documentation.

## Verification Plan

- `tools/mylite-mtr-harness probe-storage
  mylite.routed_storage_foreign_key_actions`
- `tools/mylite-mtr-harness run-storage
  mylite.routed_storage_foreign_key_actions`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- `SHOW CREATE TABLE` exposes `SET NULL` and `CASCADE` action clauses for both
  routed `InnoDB` and explicit MyLite tables.
- `ON UPDATE SET NULL` clears the child key and `ON DELETE SET NULL` leaves the
  child row alive.
- `ON UPDATE CASCADE` rewrites the child key and the forced child-key read
  finds the updated row.
- `ON DELETE CASCADE` removes the child row.
- No forbidden durable sidecar appears for the MyLite-routed schema.
- The focused storage MTR test and full storage-routed MTR list pass.

## Risks

The test is intentionally compact and does not replace the existing first-party
FK action matrix. It adds MTR-path evidence for representative action behavior
only.
