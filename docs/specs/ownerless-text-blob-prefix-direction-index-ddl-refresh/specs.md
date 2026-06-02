# Ownerless TEXT/BLOB Prefix Direction Index DDL Refresh

## Problem

Ownerless TEXT/BLOB prefix-index coverage proves long-value prefix metadata,
and prefix-direction coverage proves descending metadata on a prefixed
`VARCHAR` key part. TEXT and BLOB prefix-plus-direction indexes combine those
paths: MariaDB stores an explicit prefix length and a reverse-sort flag for the
same long-value key part, exposes both through
`information_schema.statistics`, and passes both into InnoDB native index
definitions.

MyLite needs bounded ownerless evidence that non-unique TEXT and BLOB prefix
InnoDB indexes with descending key parts created by one ownerless process
refresh already-open peers, expose `SUB_PART` plus `COLLATION = 'D'`,
disappear after `DROP INDEX`, and survive ownerless/native reopen before and
after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores the
  parsed key-part length in `KEY_PART_INFO::length`, stores
  `HA_REVERSE_SORT` when an index column is declared descending, and marks
  partial key segments with `HA_KEY_HAS_PART_KEY_SEG`.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes key-part
  direction through `information_schema.statistics.COLLATION`; existing
  prefix metadata is exposed through `SUB_PART`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` handles
  BLOB-family key parts, derives a `prefix_len` from `KEY_PART_INFO::length`,
  and passes both `prefix_len` and the `HA_REVERSE_SORT` bit to
  `dict_mem_index_add_field()`.
- `mariadb/storage/innobase/handler/handler0alter.cc:
  innobase_create_index_field_def()` records both `prefix_len` and
  descending metadata for InnoDB in-place alter index-field definitions.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for standalone
  `CREATE INDEX ... (body(5) DESC)` and
  `CREATE INDEX ... (payload(4) DESC)` plus `DROP INDEX`.
- Verify an already-open ownerless peer observes `SUB_PART = 5` and
  `COLLATION = 'D'` for the TEXT key part.
- Verify the peer observes `SUB_PART = 4` and `COLLATION = 'D'` for the BLOB
  key part.
- Verify forced-index reads and ordinary writes work while both indexes exist.
- Verify dropping both indexes refreshes the already-open peer, makes
  `FORCE INDEX` fail for both index names, and leaves the table readable.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- Unique TEXT/BLOB prefix-plus-direction indexes, charset-width edge cases,
  full algorithm/lock option matrix, crash recovery during TEXT/BLOB
  prefix-direction index DDL, and external randomized DDL oracles.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `text-blob-prefix-direction-index-ddl`:

1. A child ownerless process creates
   `app.ownerless_text_blob_prefix_direction_index_base` with `id`, latin1
   `body TEXT`, `payload BLOB`, and `weight` columns plus three rows.
2. The child creates `ownerless_text_prefix_direction_body_idx` over
   `body(5) DESC` and `ownerless_blob_prefix_direction_payload_idx` over
   `payload(4) DESC`.
3. The already-open ownerless parent observes `SUB_PART = 5` and
   `COLLATION = 'D'` for the TEXT key part, `SUB_PART = 4` and
   `COLLATION = 'D'` for the BLOB key part, uses both indexes with
   `FORCE INDEX`, inserts another matching row, and verifies aggregate
   results.
4. The child drops both indexes.
5. The parent observes both indexes absent, verifies forced-index use fails for
   both names, inserts another matching row, and checks the base table remains
   readable.
6. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if ownerless dictionary
generation, metadata flush, and existing InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless index DDL coverage to representative non-unique TEXT and
BLOB prefix-plus-direction secondary-index options. It does not claim unique
TEXT/BLOB prefix-plus-direction, charset-width, online-option matrix,
crash-recovery, or external-oracle coverage.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
secondary-index DDL inside the MyLite database directory. The final state is
verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native prefix-length plus descending-key-part metadata path for long-value
secondary indexes.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `text-blob-prefix-direction-index-ddl` plus adjacent
  `text-blob-prefix-index-ddl`, `prefix-direction-index-ddl`,
  `mixed-direction-index-ddl`, `descending-index-ddl`, and
  `unique-text-blob-prefix-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks, using focused reruns if the known intermittent InnoDB log-header
  checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `SUB_PART = 5` and
  `COLLATION = 'D'` for the TEXT prefix key part.
- The peer observes `SUB_PART = 4` and `COLLATION = 'D'` for the BLOB prefix
  key part.
- The peer can use both indexes with `FORCE INDEX` while they exist.
- After peer `DROP INDEX`, the already-open peer observes both indexes absent,
  forced-index use fails for both names, and table data remains readable.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- Unique TEXT/BLOB prefix-plus-direction indexes, charset-width edge cases,
  algorithm/lock matrices, and crash recovery during index DDL remain planned.
- External randomized DDL/RQG stress remains separate validation work.
