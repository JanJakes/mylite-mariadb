# MTR routed storage DDL lifecycle smoke

## Problem

The storage-routed MTR runner covers engine alias routing, sidecar absence,
transactions, foreign keys, and representative CHECK/generated-column
semantics. It does not yet prove the raw embedded MTR path for a compact DDL
lifecycle that combines `CREATE TABLE ... LIKE`, `CREATE TABLE ... SELECT`,
copy `ALTER`, indexed reads after rebuild, and `RENAME TABLE` under explicit
`ENGINE=InnoDB` routing.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc::Sql_cmd_create_table_like::execute()` dispatches
  ordinary create, CTAS, and `CREATE TABLE ... LIKE` paths.
- `mariadb/sql/sql_table.cc::mysql_create_like_table()` clones source table
  definitions before creating the target table.
- `mariadb/sql/sql_table.cc::mysql_alter_table()` drives copy ALTER rebuilds
  when requested by supported MyLite-routed DDL.
- `mariadb/sql/sql_table.cc::mysql_rename_table()` is the server rename entry
  point used by table rename and copy ALTER swaps.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_supported_engine_request()`
  accepts `InnoDB` as a MyLite-routed requested-engine name.
- `mariadb/storage/mylite/ha_mylite.cc` routes MyLite rename operations to
  `mylite_storage_rename_table()` for durable catalog metadata.

## Design

Add `mylite.routed_storage_ddl_lifecycle` to the storage MTR list. The test
runs with a primary `.mylite` file, enforces MyLite storage, and uses explicit
`ENGINE=InnoDB` where the SQL shape accepts an engine clause. It verifies:

- source table DDL and rows route to MyLite while preserving requested
  `InnoDB` metadata;
- `CREATE TABLE ... LIKE` preserves source table shape and accepts copied rows;
- `CREATE TABLE ... SELECT` creates an `InnoDB` target and inserts selected
  rows;
- copy `ALTER` can add a defaulted column and secondary index;
- forced indexed reads work after the copy rebuild;
- `RENAME TABLE` preserves rebuilt metadata and rows; and
- no native durable sidecars appear for the MyLite-owned schema.

## Scope

This is test and documentation work only. It exercises representative supported
DDL paths already covered by first-party CTest groups through raw embedded MTR.
It does not add new DDL support, persistence behavior, online/in-place ALTER,
partition metadata, or broader SQL rollback guarantees.

## Compatibility Impact

The storage MTR runner gains evidence that common MySQL/MariaDB DDL lifecycle
patterns continue to work when applications request `ENGINE=InnoDB` but the
active MyLite file routes storage to the MyLite handler. Broader DDL matrices
and MTR-scale comparison remain separate planned work.

## Storage And Lifecycle Impact

All durable application metadata, copied rows, CTAS rows, rebuilt index entries,
and rename state stay in the primary `.mylite` file. The existing sidecar
assertion rejects native schema directories and native engine sidecar names for
the MyLite-owned schema.

## Verification Plan

- `tools/mylite-mtr-harness run-storage mylite.routed_storage_ddl_lifecycle`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- The new storage MTR DDL lifecycle test passes.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention routed DDL lifecycle MTR coverage.

## Risks

The test intentionally keeps the lifecycle compact. It does not replace the
broader first-party routed DDL/DML CTest groups or future MTR comparison work.
