# Partition Policy

## Problem

MyLite routes supported `ENGINE=InnoDB`, `ENGINE=MyISAM`, `ENGINE=Aria`, and
default-engine table metadata to the MyLite handler, but it does not implement
MariaDB partition metadata, partition pruning, per-partition table lifecycle, or
partition-aware row and index maintenance.

Letting partitioned table DDL reach MariaDB would either fail with incidental
handler errors or risk publishing metadata that suggests partition semantics
exist. MyLite should reject partition DDL explicitly until partition support has
a storage design.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_yacc.yy`: `CREATE TABLE` accepts
  `opt_create_partitioning`, and generic `ALTER TABLE` accepts partitioning,
  remove-partitioning, and partition-management forms such as `ADD PARTITION`,
  `DROP PARTITION`, `TRUNCATE PARTITION`, `EXCHANGE PARTITION`, and
  `REORGANIZE PARTITION`.
- `mariadb/sql/sql_partition.h`: partition support is represented through
  `partition_info`, `HA_CAN_PARTITION`, `check_partition_info()`, partition
  pruning helpers, and partition metadata generation.
- `mariadb/sql/ha_partition.h`: `ha_partition` is a wrapper handler around
  underlying handlers with partition metadata create/drop/rename/change hooks,
  partition-specific locking, partition-level truncate, and multi-range read
  state.
- `mariadb/storage/mylite/ha_mylite.h`: MyLite does not advertise
  `HA_CAN_PARTITION` and has no partition metadata, partition handler, or
  partition-aware storage hooks.
- Official MariaDB partitioning documentation describes partitioning as splitting
  a table into smaller subsets where both data and indexes are partitioned, with
  DDL and maintenance surfaces documented under partitioning tables:
  <https://mariadb.com/docs/server/server-usage/partitioning-tables> and
  <https://mariadb.com/kb/en/partitioning-overview/>.

## Compatibility Impact

Partitioned tables move from an implicit planned gap to an explicit partial
compatibility policy: partition DDL fails with stable MyLite diagnostics before
MariaDB execution. Real compatibility remains planned because partitioned tables
need metadata, row routing, index maintenance, pruning, locks, and recovery that
are not present yet.

## Design

Add a `libmylite` SQL policy check for table partition DDL:

- `CREATE TABLE ... PARTITION BY ...` rejects.
- `CREATE TABLE ... PARTITIONS N` / `SUBPARTITION ...` rejects when attached to
  table DDL.
- `ALTER TABLE ... PARTITION BY ...` rejects.
- Representative partition-management forms such as `ADD PARTITION` and
  `REMOVE PARTITIONING` reject.

The detector scans SQL tokens while skipping comments, quoted identifiers, and
string literals, matching the existing foreign-key and locking policy style.
The policy lives at the public `libmylite` boundary because that is the current
product contract; raw MariaDB access is still an optional future adapter.

## Non-Goals

- Implement MariaDB partition metadata in the `.mylite` catalog.
- Implement `ha_partition` integration over MyLite table ids.
- Implement partition pruning or partition-selection reads.
- Implement partition-specific `ALTER`, `TRUNCATE`, `ANALYZE`, `OPTIMIZE`,
  `REPAIR`, or exchange/convert operations.
- Change non-partition table DDL or supported row/index behavior.

## Single-File And Embedded-Lifecycle Impact

Rejected partition DDL must not create MyLite catalog records, MariaDB durable
sidecars, or partition-owned files. The storage-smoke test keeps the same
sidecar gates after close.

## Public API And File-Format Impact

No public API or file-format change is needed. `mylite_exec()` and
`mylite_prepare()` return `MYLITE_ERROR`, leave MariaDB errno at zero, set
SQLSTATE `HY000`, and expose `unsupported partition SQL surface`.

## Storage-Engine Routing Impact

The rejection applies before storage-engine routing. This protects
`ENGINE=InnoDB` and omitted-engine DDL from implying MyLite supports
partitioned InnoDB-style table layouts.

## Build, Size, And Dependencies

No dependencies or embedded profile changes are introduced. The implementation
is first-party SQL policy code plus tests and docs.

## Test And Verification Plan

- Direct embedded API rejects `CREATE TABLE ... PARTITION BY ...` and
  representative `ALTER TABLE` partition-management forms with stable MyLite
  diagnostics.
- Prepared API rejects partitioned table DDL before MariaDB prepare.
- Storage-engine smoke rejects partitioned routed `ENGINE=InnoDB` DDL before
  catalog publication and verifies the catalog remains unchanged.
- Add a `partition` compatibility harness group.
- Run targeted direct/prepared/storage tests, the partition harness report,
  formatting, tidy, configured CTest presets, and `git diff --check`.

## Acceptance Criteria

- Partition DDL cannot be accepted through `mylite_exec()` or
  `mylite_prepare()`.
- Rejected partition DDL uses stable MyLite diagnostics rather than incidental
  MariaDB handler errors.
- Failed routed partitioned `CREATE TABLE` does not publish a table catalog
  record.
- Compatibility docs and roadmap mark partition DDL rejection as implemented
  policy while keeping real partition support planned.
- The compatibility harness can run partition evidence by name.

## Risks And Open Questions

- The lexical detector intentionally covers table DDL, not partition-selection
  reads such as `SELECT ... PARTITION (...)`. Those reads become meaningful only
  after partitioned table metadata exists.
- A future raw MariaDB adapter needs an equivalent policy or explicit lower-level
  semantics before it exposes partitioned DDL.
- Real partition support will need a separate storage design for table ids,
  per-partition row/index pages, pruning, copy ALTER, crash recovery, and lock
  behavior.
