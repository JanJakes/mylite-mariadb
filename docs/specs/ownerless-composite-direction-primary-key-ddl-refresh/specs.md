# Ownerless Composite Direction Primary-Key DDL Refresh

## Problem

Ownerless primary-key replacement coverage proves clustered-index rebuild from
`id` to `code`, and descending primary-key coverage proves reverse key-part
metadata for a single replacement clustered key part. A composite
direction-aware primary key combines clustered-index rebuild, multi-part
primary-key metadata, and mixed ASC/DESC key-part direction.

MyLite needs bounded ownerless evidence that a composite replacement primary
key created by one ownerless process refreshes already-open peers, exposes
per-key-part `COLLATION` metadata for `PRIMARY`, enforces the new composite
key, and leaves final state durable through ownerless/native reopen before and
after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_alter_table()` marks primary-key
  replacements with `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX`.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores
  `HA_REVERSE_SORT` in `KEY_PART_INFO::key_part_flag` when an index column is
  declared descending.
- `mariadb/sql/sql_table.cc` compares old/new key-part `HA_REVERSE_SORT`
  values when detecting order-changing index metadata.
- `mariadb/storage/innobase/handler/handler0alter.cc` rejects bare InnoDB
  `DROP PRIMARY KEY`, maps replacement primary-key metadata to
  `DICT_CLUSTERED | DICT_UNIQUE`, records per-field descending metadata in
  index definitions, and treats primary-key direction changes as
  order-changing DDL.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` passes each
  key part's `HA_REVERSE_SORT` bit into `dict_mem_index_add_field()`.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes primary-key
  metadata through `information_schema.statistics` with index name `PRIMARY`
  and direction through `COLLATION`.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for
  `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (tenant_id ASC, code DESC)`.
- Verify an already-open ownerless peer observes `PRIMARY` with
  `NON_UNIQUE = 0` for both key parts.
- Verify the peer observes `COLLATION = 'A'` for `tenant_id` and
  `COLLATION = 'D'` for `code`.
- Verify the old `id` column no longer appears in `PRIMARY`.
- Verify forced-index reads through `PRIMARY`, duplicate writes against the new
  composite primary key fail with MariaDB duplicate-key errno, and writes that
  duplicate only the old primary-key column succeed.
- Verify final rows and composite direction primary-key metadata through
  ownerless and native reopen before and after forced `.shm` rebuild.

Out of scope:

- AUTO_INCREMENT descending primary-key replacements, full algorithm/lock
  option matrix, crash recovery during primary-key rebuild, concurrent
  duplicate-key races, and external randomized DDL oracles.
- Unsupported bare `DROP PRIMARY KEY`; MariaDB/InnoDB requires replacement
  primary-key creation in the same ALTER.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `composite-direction-primary-key-ddl`:

1. A child ownerless process creates
   `app.ownerless_composite_direction_primary_key_base` with `id`,
   `tenant_id`, `code`, and `value` columns plus `PRIMARY KEY (id)`.
2. The child inserts three rows and runs one `ALTER TABLE` that drops the old
   primary key and adds `PRIMARY KEY (tenant_id ASC, code DESC)`.
3. The already-open ownerless parent observes `PRIMARY` on both replacement key
   parts with `NON_UNIQUE = 0`, `COLLATION = 'A'` for `tenant_id`, and
   `COLLATION = 'D'` for `code`.
4. The parent verifies the old `id` column is absent from `PRIMARY`, uses
   `FORCE INDEX (PRIMARY)`, verifies duplicate composite-key rejection, and
   inserts a row that duplicates only the old `id` value.
5. Helper assertions verify final rows, duplicate old-ID presence,
   forced-index reads, and composite direction primary-key metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if existing ownerless
dictionary refresh and InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless primary-key replacement evidence to a representative
composite clustered-index rebuild with mixed key-part direction. It does not
claim AUTO_INCREMENT descending, algorithm/lock, crash-recovery, or
external-oracle coverage for primary-key direction changes.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
clustered-index rebuild inside the MyLite database directory. The final state
is verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native clustered-index replacement path with composite ASC/DESC key-part
metadata and duplicate-key enforcement.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `composite-direction-primary-key-ddl` plus adjacent
  `primary-key-ddl`, `descending-primary-key-ddl`,
  `primary-key-autoinc-ddl`, `mixed-direction-index-ddl`, and
  `descending-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks, using focused reruns if the known intermittent InnoDB log-header
  checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `PRIMARY` on `tenant_id` and `code`
  with `NON_UNIQUE = 0`.
- The peer observes `COLLATION = 'A'` for the first key part and
  `COLLATION = 'D'` for the second key part.
- The old `id` column is absent from `PRIMARY`.
- Forced-index reads through `PRIMARY` work after the replacement.
- Duplicate inserts fail for the replacement composite key and succeed for
  values that duplicate only the old primary-key column.
- Final rows and composite direction primary-key state survive
  ownerless/native reopen before and after forced `.shm` rebuild.

## Risks And Follow-Up

- AUTO_INCREMENT descending primary-key replacements, algorithm/lock matrices,
  and crash recovery during primary-key rebuild remain planned.
- External randomized DDL/RQG stress remains separate validation work.
