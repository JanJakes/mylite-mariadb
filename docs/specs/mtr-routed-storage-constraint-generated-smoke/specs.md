# MTR routed storage constraint/generated smoke

## Problem

MyLite has first-party `libmylite` coverage for CHECK constraints and
generated columns, but the storage-routed MTR suite does not yet prove those
surfaces through raw embedded MTR execution. The gap matters because
applications commonly declare `ENGINE=InnoDB` while relying on MariaDB SQL
constraint and generated-column semantics, and explicit `ENGINE=MYLITE`
should exercise the same handler path directly.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/storage/mylite/ha_mylite.cc::mylite_supported_engine_request()`
  accepts `InnoDB` and explicit `MYLITE` as MyLite-routed requested-engine
  names.
- `mariadb/storage/mylite/ha_mylite.h` advertises
  `HA_CAN_VIRTUAL_COLUMNS`, allowing MariaDB generated-column machinery to
  operate over MyLite-routed tables.
- `mariadb/sql/unireg.cc::pack_vcols()` packs generated-column and CHECK
  expressions into the MariaDB table-definition image stored in the MyLite
  catalog.
- `mariadb/sql/table.cc::TABLE::verify_constraints()` evaluates CHECK
  expressions unless `check_constraint_checks=OFF` is set.
- `mariadb/sql/table.cc::TABLE::update_virtual_fields()` computes generated
  column values for read and write paths.
- MariaDB documents CHECK constraints and generated columns as table-definition
  features:
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/constraint>
  and
  <https://mariadb.com/docs/server/reference/sql-statements/data-definition/create/generated-columns>.

## Design

Add `mylite.routed_storage_constraints` to the storage MTR list. The test runs
with a primary `.mylite` file, enforces MyLite storage, creates explicit
`ENGINE=InnoDB` and `ENGINE=MYLITE` tables, and verifies each table's
representative:

- `SHOW CREATE TABLE` preserves the requested `InnoDB` engine and publishes
  CHECK and generated-column metadata.
- Valid rows compute virtual and stored generated values.
- Generated-column secondary and unique index lookups work through `FORCE
  INDEX`.
- Invalid CHECK writes and generated unique-key duplicates fail with MariaDB
  diagnostics.
- Updating a base column moves generated values and generated-index entries.
- Native durable schema and engine sidecars remain absent.

## Scope

This is test and documentation work only. It uses deterministic scalar
expressions that are already supported by first-party storage tests. It does
not expand the supported expression subset, add new generated-column storage
behavior, or claim exhaustive CHECK/generated parity.

## Compatibility Impact

The storage MTR runner gains evidence that routed `ENGINE=InnoDB` tables keep
representative CHECK and generated-column behavior under raw embedded MTR
execution, and that explicit `ENGINE=MYLITE` tables use the same representative
metadata, validation, generated values, and generated-index behavior. CHECK
constraints and generated columns remain partial; broader expression, SQL-mode,
and environment-sensitive matrices remain separate coverage.

## Storage And Lifecycle Impact

Generated-column and CHECK metadata remains inside the MariaDB table-definition
image stored in the primary `.mylite` file. Stored generated values and index
entries remain MyLite row/index storage. The test reuses the sidecar assertion
to reject native `.frm`, `.ibd`, MyISAM, Aria, binlog, and relay-log files.

## Verification Plan

- `tools/mylite-mtr-harness run-storage mylite.routed_storage_constraints`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- The new storage MTR constraints test passes.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention routed CHECK/generated MTR coverage.

## Verification Results

- `tools/mylite-mtr-harness probe-storage mylite.routed_storage_constraints`
  passed after adding explicit `ENGINE=MYLITE` CHECK/generated coverage.

## Risks

The test is intentionally representative. It proves one stable routed
constraint/generated path through MTR, not full MariaDB expression coverage.
