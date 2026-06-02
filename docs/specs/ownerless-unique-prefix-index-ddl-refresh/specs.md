# Ownerless Unique Prefix Index DDL Refresh

## Problem

Ownerless prefix-index coverage proves shortened key-part metadata, and
unique-index coverage proves uniqueness enforcement. Unique prefix indexes
combine both behaviors: MariaDB records a shortened key-part length, marks the
index unique, exposes `SUB_PART` and `NON_UNIQUE` through
`information_schema.statistics`, and enforces uniqueness over the indexed
prefix through InnoDB.

MyLite needs bounded ownerless evidence that a unique prefix InnoDB index
created by one ownerless process refreshes already-open peers, rejects
duplicate prefixes while present, disappears after `DROP INDEX`, and survives
ownerless/native reopen before and after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores the
  parsed prefix length in `KEY_PART_INFO::length`, marks partial key segments
  with `HA_KEY_HAS_PART_KEY_SEG`, and records uniqueness through
  `KEY::UNIQUE`/`HA_NOSAME`.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes unique-index
  metadata as `NON_UNIQUE = 0` and prefix length through
  `information_schema.statistics.SUB_PART`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` compares the
  key-part length with the table field length and passes the resulting
  `prefix_len` into `dict_mem_index_add_field()` for InnoDB index creation.
- `mariadb/storage/innobase/handler/handler0alter.cc` records the same prefix
  length in InnoDB in-place alter index-field definitions.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for standalone
  `CREATE UNIQUE INDEX ... (code(4))` and `DROP INDEX`.
- Verify an already-open ownerless peer observes `NON_UNIQUE = 0` and
  `SUB_PART = 4`.
- Verify duplicate-prefix rejection while the unique prefix index exists.
- Verify dropping the index refreshes the already-open peer, makes
  `FORCE INDEX` fail, permits the formerly duplicate prefix, and leaves the
  table readable.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- Charset-width edge cases, full algorithm/lock option matrix, crash recovery
  during unique prefix-index DDL, and external randomized DDL oracles. Unique
  prefix-plus-direction secondary-index DDL is covered separately by
  `ownerless-unique-prefix-direction-index-ddl-refresh`, and unique TEXT/BLOB
  prefix indexes are covered separately by
  `ownerless-unique-text-blob-prefix-index-ddl-refresh`.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `unique-prefix-index-ddl`:

1. A child ownerless process creates
   `app.ownerless_unique_prefix_index_base` with `id`, `code`, and `weight`
   columns and three rows whose four-character prefixes are distinct.
2. The child creates `ownerless_unique_prefix_code_idx` as a standalone unique
   prefix secondary index over `code(4)`.
3. The already-open ownerless parent observes `NON_UNIQUE = 0` and
   `SUB_PART = 4`, uses `FORCE INDEX`, verifies duplicate-prefix rejection,
   inserts a distinct-prefix row, and verifies the forced-index aggregate
   includes it.
4. The child drops the index.
5. The parent observes index absence, verifies `FORCE INDEX` fails, inserts a
   row that would have duplicated an indexed prefix, and checks the base table
   remains readable.
6. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if ownerless dictionary
generation, metadata flush, and existing InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless index DDL coverage to a representative unique prefix
secondary-index option. It does not claim charset-width or online-option matrix
coverage for unique prefix indexes; unique prefix-plus-direction DDL and unique
TEXT/BLOB prefix DDL are covered by separate focused slices.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
secondary-index DDL inside the MyLite database directory. The final state is
verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native unique prefix secondary-index metadata and enforcement path.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `unique-prefix-index-ddl` plus adjacent `prefix-index-ddl`,
  `unique-index-ddl`, `unique-descending-index-ddl`, `index-ddl`, and
  `rename-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks, using focused reruns if the known intermittent InnoDB log-header
  checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `NON_UNIQUE = 0` and `SUB_PART = 4`
  for the unique prefix index.
- Duplicate-prefix insertion fails while the unique prefix index exists.
- The peer can use the unique prefix index with `FORCE INDEX` while it exists.
- After peer `DROP INDEX`, the already-open peer observes index absence,
  forced-index use fails, a formerly duplicate prefix can be inserted, and
  table data remains readable.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- Charset-width edge cases, algorithm/lock matrices, and crash recovery during
  index DDL remain planned. Unique prefix-plus-direction secondary-index DDL is
  covered separately by `ownerless-unique-prefix-direction-index-ddl-refresh`,
  and unique TEXT/BLOB prefix DDL is covered separately by
  `ownerless-unique-text-blob-prefix-index-ddl-refresh`.
- External randomized DDL/RQG stress remains separate validation work.
