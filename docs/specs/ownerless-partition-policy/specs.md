# Ownerless Partition Policy

## Problem

Ownerless read/write mode coordinates ordinary InnoDB table, index, and covered
DDL paths through directory-backed locks, dictionary generations, page-version
WAL, and native checkpoint evidence. Partitioned tables are a broader DDL and
file-lifecycle class: a single SQL table can map to partition metadata plus
multiple native engine files, and partition maintenance can add, drop,
truncate, reorganize, or rename those native partition files.

Until MyLite carries durable ownerless file-lifecycle metadata for partition
objects, ownerless partitioned table DDL must fail explicitly instead of
accidentally entering MariaDB partition handler paths.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- MariaDB partition documentation describes partitioned tables as multiple
  files and notes that InnoDB commonly creates one `.ibd` file per partition,
  with `.frm`, `.par`, and `table_name#P#partition_name.ext` file naming.
- MariaDB partition documentation also describes `CREATE TABLE` partitioning
  plus `ALTER TABLE` operations that add, drop, reorganize, coalesce, truncate,
  and remove partitions.
- `mariadb/sql/handler.cc:get_ha_partition()` constructs the partition handler
  wrapper, and `handler::ha_create_partitioning_metadata()`,
  `handler::ha_change_partitions()`, `handler::ha_drop_partitions()`, and
  `handler::ha_rename_partitions()` expose storage-engine partition lifecycle
  operations.
- `mariadb/sql/sql_partition.h` declares partition metadata and maintenance
  helpers such as `fast_alter_partition_table()`,
  `set_part_state()`, `generate_partition_syntax()`, and partition iterators.
- MyLite ownerless DDL currently proves ordinary InnoDB create, rename,
  truncate, drop, same-name recreate, CTAS, `CREATE TABLE ... LIKE`, ordinary
  secondary indexes, unique indexes, primary-key replacement, foreign keys,
  CHECK constraints, generated columns, ordinary online/in-place index alter,
  and explicit instant column metadata. It does not yet record durable
  partition file lifecycle metadata.

## Scope And Non-Goals

- Reject ownerless `CREATE TABLE` and `ALTER TABLE` statements that request
  partitioning or partition maintenance.
- Preserve ordinary exclusive-mode MariaDB partition behavior; the policy is
  ownerless read/write only.
- Verify rejected statements do not create partitioned tables or partition
  metadata under an ownerless database directory.
- Do not implement partition coordination, partition-file replay, partition
  pruning semantics, partition maintenance, or external MariaDB/RQG partition
  stress in this slice.

## Design

- Add an ownerless SQL policy predicate that scans `CREATE` and `ALTER`
  statements after the `TABLE` keyword.
- Reject identifier tokens `PARTITION`, `PARTITIONING`, `PARTITIONS`,
  `SUBPARTITION`, `SUBPARTITIONING`, and `SUBPARTITIONS` in that table-DDL
  scope.
- Return a MyLite policy error before MariaDB dispatch, keeping MariaDB errno
  at zero like other ownerless unsupported-policy checks.
- Add a focused `partition-policy` ownerless SQL selector that:
  - creates a normal InnoDB base table,
  - rejects `CREATE TABLE ... PARTITION BY RANGE`,
  - rejects `CREATE TABLE ... PARTITION BY HASH ... PARTITIONS`,
  - rejects `CREATE TABLE ... SUBPARTITION BY ... SUBPARTITIONS`,
  - rejects `ALTER TABLE ... PARTITION BY`,
  - rejects `ALTER TABLE ... ADD PARTITION`,
  - rejects `ALTER TABLE ... TRUNCATE PARTITION`,
  - rejects `ALTER TABLE ... REMOVE PARTITIONING`, and
  - verifies no rejected tables or partition metadata appear through ownerless
    or native reopen before and after forced `.shm` rebuild.

## Compatibility Impact

Ownerless read/write partitioned table DDL becomes explicitly unsupported.
This is safer than inheriting MariaDB behavior accidentally, because MyLite
does not yet prove partition file lifecycle, partition metadata refresh, or
partition replay across ownerless peers.

Ordinary non-ownerless embedded behavior is unchanged.

## Directory And Lifecycle Impact

No new files are created by supported ownerless paths. The policy prevents
ownerless execution from creating `.par` or per-partition native engine files
before the directory lifecycle can track them.

## Native Storage Impact

The slice does not change InnoDB storage behavior for supported ordinary
tables. It blocks partition handler storage paths in ownerless mode until a
future design covers partition metadata, partition file names, partition
maintenance, and no-live replay.

## Binary Size And Dependencies

No dependency or license changes. The binary impact is one policy predicate and
focused test code.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `partition-policy` in `embedded-dev`.
- Build and run focused `partition-policy` in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL, hook ownerless SQL label,
  ownerless stress, `format-check`, and diff checks.

## Acceptance Criteria

- Ownerless partitioned table DDL fails with a MyLite policy error before
  MariaDB dispatch.
- Rejected partitioned table DDL leaves no partitioned table metadata or
  rejected table definitions visible through ownerless or native reopen before
  or after forced `.shm` rebuild.
- Existing ownerless DDL, SQL, hook, and stress coverage remains green.

## Risks And Follow-Up

- The token policy is conservative for ownerless table DDL and may reject
  unusual partition-keyword table syntax that MariaDB could otherwise parse.
  Quoted identifiers are not treated as identifiers by the policy tokenizer.
- Full ownerless partition support needs durable file lifecycle metadata for
  partition files, partition-aware native replay, partition maintenance tests,
  and external-oracle stress.
