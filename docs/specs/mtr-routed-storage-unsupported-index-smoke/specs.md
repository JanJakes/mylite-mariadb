# MTR routed storage unsupported index smoke

## Problem

The first-party compatibility harness proves that MyLite rejects unsupported
index classes before catalog publication, but the raw storage-routed MTR list
does not yet exercise that policy through MariaDB's embedded test runner.
MyLite should keep failing unsupported index definitions explicitly instead of
publishing table metadata for index classes the storage layer cannot maintain.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses ordinary, `FULLTEXT`, and `SPATIAL` key
  definitions for table DDL and index DDL.
- `mariadb/sql/sql_table.cc::mysql_create_table_no_lock()` prepares table and
  key definitions before invoking the storage engine create path.
- `mariadb/storage/mylite/ha_mylite.cc::ha_mylite::create()` rejects unsupported
  table shapes before MyLite catalog publication.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_table_supports_row_write()`
  rejects MariaDB high-level indexes before table publication; vector indexes
  are represented as high-level indexes outside ordinary `TABLE_SHARE::keys`.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_key_is_supported()` rejects
  `HA_FULLTEXT_legacy`, `HA_SPATIAL_legacy`, hash, non-B-tree, unbounded
  BLOB/TEXT, and oversized key shapes.
- `mariadb/sql/handler.cc::handler::print_error()` maps
  `HA_ERR_UNSUPPORTED` to `ER_UNSUPPORTED_EXTENSION`.
- Raw MTR execution can also fail earlier in MariaDB's SQL layer for index
  classes not advertised by the selected engine, such as
  `ER_TABLE_CANT_HANDLE_FT` for FULLTEXT and `ER_CHECK_NOT_IMPLEMENTED` for
  GEOMETRY-backed SPATIAL definitions.

## Design

Extend `mylite.routed_storage_unsupported_indexes` in the storage MTR list. The
test runs with a primary `.mylite` file and enforced MyLite storage. It creates
supported tables, then verifies representative unsupported index requests fail
without corrupting those tables:

- `CREATE TABLE ... FULLTEXT KEY ... ENGINE=InnoDB`;
- `CREATE TABLE ... SPATIAL KEY ... ENGINE=InnoDB`;
- `CREATE TABLE ... VECTOR KEY ... ENGINE=InnoDB`;
- `CREATE TABLE ... UNIQUE KEY` over unbounded `TEXT`;
- `ALTER TABLE ... ADD FULLTEXT KEY`;
- `ALTER TABLE ... ADD SPATIAL KEY`; and
- standalone `CREATE UNIQUE INDEX` over unbounded `TEXT`.

The test checks that supported tables remain visible and queryable, failed
standalone long-unique index DDL leaves the target table without that index,
failed initial-create table names are absent from `INFORMATION_SCHEMA.TABLES`,
and the standard sidecar assertion still passes.

## Scope

This is test and documentation work only. It does not implement FULLTEXT,
SPATIAL, vector, expression, hash, unbounded BLOB/TEXT, or multi-page index
support, and it does not change MyLite's unsupported-index diagnostics.

## Compatibility Impact

The storage MTR runner gains direct evidence that raw embedded MyLite storage
sessions reject representative unsupported index classes before publishing
failed table metadata or mutating existing supported table metadata. This backs
the existing compatibility contract that unsupported index classes fail
explicitly while supported primary, unique, secondary, and bounded prefix
indexes continue to work.

## Storage And Lifecycle Impact

Failed initial creates must not publish MyLite catalog records, row/index pages,
or native engine sidecars. Failed `ALTER TABLE ... ADD ... KEY` requests must
leave the existing table queryable with its prior supported index metadata.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No binary-size or dependency impact; this adds only MTR test and documentation
coverage.

## Verification Plan

- `tools/mylite-mtr-harness run-storage
  mylite.routed_storage_unsupported_indexes`
- `tools/mylite-mtr-harness run-storage`
- `tools/mylite-mtr-harness coverage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- Unsupported FULLTEXT, SPATIAL, vector, and unbounded `TEXT` unique
  initial-create requests fail.
- Unsupported FULLTEXT and SPATIAL `ALTER TABLE ... ADD KEY` requests fail.
- Unsupported standalone unbounded `TEXT` unique index requests fail without
  publishing index metadata on the supported table.
- Failed initial-create table names are not visible through
  `INFORMATION_SCHEMA.TABLES`.
- Supported tables remain queryable with supported metadata intact.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention raw storage-routed unsupported-index
  coverage.

## Risks

The raw MTR test remains representative. Diagnostics differ depending on
whether MariaDB fails during SQL-layer index capability checks or the MyLite
handler rejects a key shape through `HA_ERR_UNSUPPORTED`. Expression, hash, and
broader full BLOB/TEXT index policy stays covered by first-party compatibility
groups until those shapes have stable raw-MTR parser and profile behavior worth
adding to the curated storage list.
