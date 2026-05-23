# MTR routed storage DDL lifecycle smoke

## Problem

The storage-routed MTR runner covers engine alias routing, sidecar absence,
transactions, foreign keys, and representative CHECK/generated-column
semantics. It does not yet prove the raw embedded MTR path for a compact DDL
lifecycle that combines `CREATE TABLE ... LIKE`, `CREATE TABLE ... SELECT`,
`CREATE OR REPLACE TABLE` success and a pre-drop failed `LIKE` replacement,
copy `ALTER`, indexed reads after rebuild, and `RENAME TABLE` under explicit
`ENGINE=InnoDB` routing and explicit `ENGINE=MYLITE` requests.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc::Sql_cmd_create_table_like::execute()` dispatches
  ordinary create, CTAS, and `CREATE TABLE ... LIKE` paths.
- `mariadb/sql/sql_table.cc::mysql_create_like_table()` clones source table
  definitions before creating the target table.
- `mariadb/sql/sql_table.cc` handles `CREATE OR REPLACE TABLE` by dropping the
  old target before routing the replacement through the normal create path.
- Missing `LIKE` sources are rejected before the replacement drop reaches the
  MyLite handler in raw embedded MTR execution.
- `mariadb/sql/sql_table.cc::mysql_alter_table()` drives copy ALTER rebuilds
  when requested by supported MyLite-routed DDL.
- `mariadb/sql/sql_table.cc::mysql_rename_table()` is the server rename entry
  point used by table rename and copy ALTER swaps.
- `mariadb/storage/mylite/ha_mylite.cc::mylite_supported_engine_request()`
  accepts `MYLITE` directly and `InnoDB` as a MyLite-routed requested-engine
  name.
- `mariadb/storage/mylite/ha_mylite.cc` routes MyLite rename operations to
  `mylite_storage_rename_table()` for durable catalog metadata.

## Design

Add `mylite.routed_storage_ddl_lifecycle` to the storage MTR list. The test
runs with a primary `.mylite` file, enforces MyLite storage, and uses explicit
`ENGINE=InnoDB` plus explicit `ENGINE=MYLITE` where the SQL shape accepts an
engine clause. It verifies:

- source table DDL and rows route to MyLite while preserving requested
  `InnoDB` and `MYLITE` metadata;
- `CREATE TABLE ... LIKE` preserves source table shape and accepts copied rows;
- `CREATE TABLE ... SELECT` creates requested-engine targets and inserts
  selected rows;
- routed `CREATE OR REPLACE TABLE` plain, LIKE, and CTAS replacement removes
  old SQL-visible definitions and rows before publishing supported replacement
  metadata;
- a pre-drop missing-source `CREATE OR REPLACE TABLE ... LIKE` failure
  preserves the old routed target and its supported indexes;
- explicit MyLite `CREATE OR REPLACE TABLE` replacement follows the same
  visible lifecycle;
- copy `ALTER` can add a defaulted column and secondary index;
- forced indexed reads work after the copy rebuild;
- `RENAME TABLE` preserves rebuilt metadata and rows; and
- no native durable sidecars appear for the MyLite-owned schema.

## Scope

This is test and documentation work only. It exercises representative supported
DDL paths already covered by first-party CTest groups through raw embedded MTR.
It does not add new DDL support, persistence behavior, online/in-place ALTER,
partition metadata, exhaustive OR REPLACE failure matrices, or broader SQL
rollback guarantees.

## Compatibility Impact

The storage MTR runner gains evidence that common MySQL/MariaDB DDL lifecycle
patterns continue to work when applications request `ENGINE=InnoDB` but the
active MyLite file routes storage to the MyLite handler, and when applications
request `ENGINE=MYLITE` directly. It now also checks the raw embedded
drop-then-create OR REPLACE path for representative supported replacements and
the pre-drop missing-source `LIKE` failure path. Broader DDL matrices and
MTR-scale comparison remain separate planned work.

## Storage And Lifecycle Impact

All durable application metadata, copied rows, CTAS rows, replacement
definitions, replacement rows, rebuilt index entries, and rename state stay in
the primary `.mylite` file. The existing sidecar assertion rejects native schema
directories and native engine sidecar names for the MyLite-owned schema.
The covered missing-source replacement failure leaves the old table definition,
rows, and supported indexes visible because MariaDB rejects it before dropping
the target in this raw embedded path.

## Verification Plan

- `tools/mylite-mtr-harness run-storage mylite.routed_storage_ddl_lifecycle`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- The new storage MTR DDL lifecycle test passes.
- Plain, LIKE, CTAS, and explicit MyLite OR REPLACE replacements remove old
  SQL-visible definitions and rows while keeping supported replacement indexes
  usable.
- The missing-source LIKE replacement failure preserves the old target table
  and supported index reads.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention routed and explicit MyLite DDL
  lifecycle MTR coverage.

## Risks

The test intentionally keeps the lifecycle compact. It does not replace the
broader first-party routed DDL/DML CTest groups or future MTR comparison work.
