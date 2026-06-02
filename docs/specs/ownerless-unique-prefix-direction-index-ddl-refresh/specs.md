# Ownerless Unique Prefix Direction Index DDL Refresh

## Problem

Ownerless prefix-direction coverage proves shortened key-part length plus
descending metadata, and unique-prefix coverage proves uniqueness enforcement
over shortened key parts. A unique prefix-plus-direction index combines all of
those properties: MariaDB records uniqueness, a shortened key-part length, and
per-key-part direction, exposes them through
`information_schema.statistics`, and passes them to InnoDB native index
definitions.

MyLite needs bounded ownerless evidence that a unique prefixed descending
InnoDB secondary index created by one ownerless process refreshes already-open
peers, rejects duplicate prefix-plus-score keys while present, disappears after
`DROP INDEX`, and survives ownerless/native reopen before and after forced
shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores the
  parsed prefix length in `KEY_PART_INFO::length`, marks partial key segments
  with `HA_KEY_HAS_PART_KEY_SEG`, stores `HA_REVERSE_SORT` for descending key
  parts, and records uniqueness through `KEY::UNIQUE`/`HA_NOSAME`.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes unique-index
  metadata as `NON_UNIQUE = 0`, prefix length through `SUB_PART`, and
  key-part direction through `COLLATION`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` passes the
  key-part `prefix_len` and `HA_REVERSE_SORT` bit into
  `dict_mem_index_add_field()`.
- `mariadb/storage/innobase/handler/handler0alter.cc` records prefix lengths
  and descending metadata in InnoDB in-place alter index-field definitions.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for standalone
  `CREATE UNIQUE INDEX ... (code(4) DESC, score ASC)` and `DROP INDEX`.
- Verify an already-open ownerless peer observes `NON_UNIQUE = 0`.
- Verify the peer observes `SUB_PART = 4` plus `COLLATION = 'D'` on the
  prefixed key part and `COLLATION = 'A'` on the second key part.
- Verify duplicate prefix-plus-score rejection while the index exists.
- Verify dropping the index refreshes the already-open peer, makes
  `FORCE INDEX` fail, permits the formerly duplicate prefix-plus-score key, and
  leaves the table readable.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- TEXT/BLOB unique prefix-plus-direction indexes, charset-width edge cases,
  full algorithm/lock option matrix, crash recovery during unique
  prefix-direction index DDL, and external randomized DDL oracles.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `unique-prefix-direction-index-ddl`:

1. A child ownerless process creates
   `app.ownerless_unique_prefix_direction_base` with `id`, latin1 `code`,
   `score`, and `weight` columns plus three rows.
2. The child creates `ownerless_unique_prefix_direction_idx` over
   `code(4) DESC, score ASC`.
3. The already-open ownerless parent observes `NON_UNIQUE = 0`, `SUB_PART = 4`
   and `COLLATION = 'D'` for sequence 1, and `COLLATION = 'A'` for sequence 2,
   uses `FORCE INDEX`, verifies duplicate prefix-plus-score rejection, inserts
   a distinct score with the same prefix, and verifies aggregate results.
4. The child drops the index.
5. The parent observes index absence, verifies forced-index use fails, inserts
   a formerly duplicate prefix-plus-score row, and checks the base table
   remains readable.
6. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if ownerless dictionary
generation, metadata flush, and existing InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless index DDL coverage to a representative unique
prefix-plus-direction secondary-index option. It does not claim TEXT/BLOB,
charset-width, online-option matrix, crash-recovery, or external-oracle
coverage for unique prefix-plus-direction indexes.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
secondary-index DDL inside the MyLite database directory. The final state is
verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native unique prefix-length plus descending-key-part metadata and enforcement
path for an ordinary secondary index.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `unique-prefix-direction-index-ddl` plus adjacent
  `prefix-direction-index-ddl`, `unique-prefix-index-ddl`,
  `unique-descending-index-ddl`, `prefix-index-ddl`, and
  `mixed-direction-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks, using focused reruns if the known intermittent InnoDB log-header
  checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `NON_UNIQUE = 0` for both key parts.
- The peer observes `SUB_PART = 4` and `COLLATION = 'D'` for the prefixed
  descending key part.
- The peer observes `COLLATION = 'A'` for the second key part.
- Duplicate prefix-plus-score insertion fails while the index exists.
- After peer `DROP INDEX`, the already-open peer observes index absence,
  forced-index use fails, a formerly duplicate key can be inserted, and table
  data remains readable.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- TEXT/BLOB unique prefix-plus-direction indexes, charset-width edge cases,
  algorithm/lock matrices, and crash recovery during index DDL remain planned.
- External randomized DDL/RQG stress remains separate validation work.
