# MTR routed storage partition policy smoke

## Problem

MyLite deliberately rejects partitioned table DDL at the public `libmylite`
boundary because it does not yet have partition metadata, partition routing,
per-partition catalog lifecycle, or partition-aware row/index maintenance. The
raw storage-routed MTR list bypasses that public preflight, so it should still
prove the embedded storage profile does not accidentally accept partitioned
routed DDL or publish failed partition table metadata.

## Source Findings

- MariaDB base: `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_yacc.yy` parses `CREATE TABLE ... PARTITION BY ...` and
  partition-management `ALTER TABLE` forms.
- `mariadb/sql/sql_partition.h` models partition metadata through
  `partition_info`, `HA_CAN_PARTITION`, and partition validation helpers.
- `mariadb/sql/ha_partition.h` defines MariaDB's partition wrapper handler
  with partition-specific metadata and maintenance hooks.
- `mariadb/storage/mylite/ha_mylite.h` does not advertise `HA_CAN_PARTITION`
  and MyLite has no partition handler or partition-aware storage hooks.
- The default embedded and storage-smoke profiles omit the native partition
  wrapper, so raw MTR diagnostics may come from MariaDB's disabled partition
  profile or from handler capability checks rather than from `libmylite`
  policy preflight.

## Design

Add `mylite.routed_storage_partitions` to the storage MTR list. The test runs
with a primary `.mylite` file and enforced MyLite storage. It creates one
ordinary routed table, verifies representative partition-definition forms fail,
and then checks the ordinary table remains visible and queryable while failed
partitioned initial-create metadata is absent:

- `CREATE TABLE ... ENGINE=InnoDB PARTITION BY HASH ...`;
- `ALTER TABLE ... PARTITION BY HASH ...`.

The test intentionally accepts more than one MariaDB error symbol because raw
MTR bypasses MyLite's public SQL policy layer. The invariant is failure without
MyLite catalog publication or durable sidecars.

## Scope

This is test and documentation work only. It does not change the public
partition policy detector, add partition metadata, enable the native partition
wrapper, or implement partition routing.

## Compatibility Impact

The storage MTR runner gains direct evidence that partitioned routed DDL remains
unsupported under raw embedded storage execution. Public `libmylite` direct and
prepared SQL remain the authoritative compatibility surface for stable
partition diagnostics.

## Storage And Lifecycle Impact

Failed raw partition DDL must not publish MyLite catalog records for failed
initial creates or create native partition sidecars. Existing ordinary table
metadata and rows must remain queryable.

## Public API And File-Format Impact

No public `libmylite` API change and no file-format change.

## Binary-Size And Dependency Impact

No binary-size or dependency impact; this adds only MTR test and documentation
coverage.

## Verification Plan

- `tools/mylite-mtr-harness probe-storage mylite.routed_storage_partitions`
- `tools/mylite-mtr-harness run-storage mylite.routed_storage_partitions`
- `tools/mylite-mtr-harness run-storage`
- `bash -n tools/mylite-mtr-harness`
- `git diff --check`

## Acceptance Criteria

- Representative partitioned initial-create and partition-management `ALTER`
  forms fail under raw storage-routed MTR.
- The supported routed table remains queryable.
- Failed partitioned initial-create table names are not visible through
  `INFORMATION_SCHEMA.TABLES`.
- The full storage-routed MTR list passes.
- Compatibility docs and roadmap mention raw storage-routed partition rejection
  coverage without claiming public `libmylite` diagnostics for raw MTR.

## Risks

Raw MTR diagnostics can change if MariaDB's disabled partition profile changes
which layer rejects first. MariaDB may also treat some partition-management
forms on non-partitioned raw tables as no-ops; the public direct/prepared APIs
continue to own stable MyLite partition-management diagnostics. The raw test
therefore asserts failure and metadata absence only for partition-definition
forms.
