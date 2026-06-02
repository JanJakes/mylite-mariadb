# Ownerless Descending Primary-Key DDL Refresh

## Problem

Ownerless primary-key replacement coverage proves an ordinary clustered-index
rebuild from `id` to `code`, and descending secondary-index coverage proves
reverse key-part metadata for non-clustered indexes. A descending primary-key
replacement combines both: MariaDB records a reverse-sort flag on the
replacement clustered key part, InnoDB rebuilds the clustered index, and
already-open peers must refresh dictionary metadata before enforcing the new
primary key.

MyLite needs bounded ownerless evidence that a descending replacement primary
key created by one ownerless process refreshes already-open peers, exposes
`COLLATION = 'D'` for `PRIMARY`, enforces the new key, and leaves final state
durable through ownerless/native reopen before and after forced shared-memory
rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_alter_table()` marks primary-key
  replacements with `ALTER_DROP_PK_INDEX` and `ALTER_ADD_PK_INDEX`.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores
  `HA_REVERSE_SORT` in `KEY_PART_INFO::key_part_flag` when an index column is
  declared descending.
- `mariadb/storage/innobase/handler/handler0alter.cc` rejects bare InnoDB
  `DROP PRIMARY KEY`, maps replacement primary-key metadata to
  `DICT_CLUSTERED | DICT_UNIQUE`, and treats primary-key ASC/DESC changes as
  order-changing DDL.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` passes each
  key part's `HA_REVERSE_SORT` bit into `dict_mem_index_add_field()`.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes primary-key
  metadata through `information_schema.statistics` with index name `PRIMARY`
  and direction through `COLLATION`.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for
  `ALTER TABLE ... DROP PRIMARY KEY, ADD PRIMARY KEY (code DESC)`.
- Verify an already-open ownerless peer observes `PRIMARY` on `code` with
  `NON_UNIQUE = 0` and `COLLATION = 'D'`.
- Verify the old `id` column no longer appears in `PRIMARY`.
- Verify forced-index reads through `PRIMARY`, duplicate writes against the new
  primary key fail with MariaDB duplicate-key errno, and writes that duplicate
  only the old primary-key column succeed.
- Verify final rows and descending primary-key metadata through ownerless and
  native reopen before and after forced `.shm` rebuild.

Out of scope:

- Composite descending primary keys, AUTO_INCREMENT descending primary-key
  replacements, full algorithm/lock option matrix, crash recovery during
  primary-key rebuild, concurrent duplicate-key races, and external randomized
  DDL oracles.
- Unsupported bare `DROP PRIMARY KEY`; MariaDB/InnoDB requires replacement
  primary-key creation in the same ALTER.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `descending-primary-key-ddl`:

1. A child ownerless process creates
   `app.ownerless_descending_primary_key_base` with `id`, `code`, and `value`
   columns plus `PRIMARY KEY (id)`.
2. The child inserts three rows and runs one `ALTER TABLE` that drops the old
   primary key and adds `PRIMARY KEY (code DESC)`.
3. The already-open ownerless parent observes `PRIMARY` on `code` with
   `COLLATION = 'D'`, verifies the old `id` column is absent from `PRIMARY`,
   uses `FORCE INDEX (PRIMARY)`, verifies duplicate `code` rejection, and
   inserts a row that duplicates only the old `id` value.
4. Helper assertions verify final rows, duplicate old-ID presence, forced-index
   reads, and descending primary-key metadata through ownerless/native reopen
   before and after forced shared-memory rebuild.

The slice should require no product-code change if existing ownerless
dictionary refresh and InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless primary-key replacement evidence to a representative
descending clustered-index rebuild. It does not claim composite, AUTO_INCREMENT,
algorithm/lock, crash-recovery, or external-oracle coverage for primary-key
direction changes.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
clustered-index rebuild inside the MyLite database directory. The final state
is verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native clustered-index replacement path with descending key-part metadata.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `descending-primary-key-ddl` plus adjacent `primary-key-ddl`,
  `primary-key-autoinc-ddl`, `descending-index-ddl`, and
  `unique-descending-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks, using focused reruns if the known intermittent InnoDB log-header
  checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `PRIMARY` on `code` with
  `COLLATION = 'D'` and `NON_UNIQUE = 0`.
- The old `id` column is absent from `PRIMARY`.
- Forced-index reads through `PRIMARY` work after the replacement.
- Duplicate inserts fail for the replacement key and succeed for values that
  duplicate only the old primary-key column.
- Final rows and descending primary-key state survive ownerless/native reopen
  before and after forced `.shm` rebuild.

## Risks And Follow-Up

- Composite descending primary keys, AUTO_INCREMENT descending primary-key
  replacements, algorithm/lock matrices, and crash recovery during primary-key
  rebuild remain planned.
- External randomized DDL/RQG stress remains separate validation work.
