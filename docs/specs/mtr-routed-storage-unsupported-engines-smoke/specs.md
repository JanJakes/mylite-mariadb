# MTR routed storage unsupported engines smoke

## Problem

The storage-routed MTR list proves supported engine alias routing, but it does
not yet prove that unsupported engine requests fail through the raw embedded MTR
path without publishing MyLite catalog metadata. MyLite should keep rejecting
native or external engine files that do not fit the single-primary-file product
shape, including CSV, SEQUENCE, and external plugin-style engines.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/handler.cc::ha_resolve_by_name()` emits
  `ER_UNKNOWN_STORAGE_ENGINE` for unresolved engine names before handler
  creation.
- `mariadb/sql/sql_table.cc::mysql_create_table_no_lock()` resolves table
  options and invokes the selected handler create path for supported engines.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::create()` rejects requested
  engine names that `mylite_supported_engine_request()` does not accept before
  MyLite table metadata publication.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_supported_engine_request()`
  allows only default/MyLite plus the documented routed aliases and volatile or
  row-discard aliases.

## Design

Add `mylite.routed_storage_unsupported_engines` to the storage MTR list. The
test runs with a primary `.mylite` file and enforced MyLite storage. It creates
one supported table, then verifies representative unsupported requests fail:

- `CREATE TABLE ... ENGINE=CSV`;
- `CREATE TABLE ... ENGINE=SEQUENCE`;
- `CREATE TABLE ... ENGINE=ARCHIVE`;
- MariaDB's no-equals spelling for a known unsupported engine; and
- `ALTER TABLE ... ENGINE=CSV` against an existing MyLite-routed table.

The test then checks that only the supported table is visible and that failed
table names did not enter `INFORMATION_SCHEMA.TABLES`, followed by the standard
sidecar assertion.

## Scope

This is test and documentation work only. It does not add new policy checks or
change diagnostics. It intentionally keeps the MTR matrix representative because
the first-party `server-surface` and unsupported-engine groups already cover
broader direct/prepared SQL policy.

## Compatibility Impact

The storage MTR runner gains direct evidence that unsupported table-engine
requests continue to fail in an embedded MyLite storage session without
creating durable sidecars or catalog-backed tables.

## Storage And Lifecycle Impact

Failed requests must not publish MyLite catalog records, row/index pages, or
native engine sidecars. The existing sidecar assertion remains the final
file-lifecycle gate for the schema.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No binary-size or dependency impact; this adds only MTR test and documentation
coverage.

## Verification Plan

- `tools/mylite-mtr-harness probe-storage
  mylite.routed_storage_unsupported_engines`
- `tools/mylite-mtr-harness run-storage
  mylite.routed_storage_unsupported_engines`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- Unsupported `CREATE TABLE` and `ALTER TABLE` engine requests fail.
- Failed table names are not visible through `SHOW TABLES` or
  `INFORMATION_SCHEMA.TABLES`.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention raw storage-routed unsupported-engine
  request coverage.

## Risks

Diagnostics differ depending on whether MariaDB fails during engine-name
resolution or the MyLite handler rejects a known-but-unsupported request. The
test should assert stable failure for representative names without requiring
all unsupported engines to share one SQL error symbol.
