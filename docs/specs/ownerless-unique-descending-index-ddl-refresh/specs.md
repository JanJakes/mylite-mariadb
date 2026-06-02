# Ownerless Unique Descending Index DDL Refresh

## Problem

Ownerless unique-index coverage proves multi-column uniqueness enforcement,
and descending-index coverage proves reverse key-part metadata. Unique
descending indexes remain a separate combined option class: MariaDB records
both uniqueness and key-part direction in index metadata, exposes both through
`information_schema.statistics`, and enforces uniqueness through InnoDB native
secondary indexes.

MyLite needs bounded ownerless evidence that a unique descending InnoDB index
created by one ownerless process refreshes already-open peers, enforces
duplicates while present, disappears after `DROP INDEX`, and survives
ownerless/native reopen before and after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores
  uniqueness in `KEY::UNIQUE`/`HA_NOSAME` metadata and stores
  `HA_REVERSE_SORT` on descending key parts.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` creates
  InnoDB indexes from MariaDB `KEY` metadata and passes each key part's
  `HA_REVERSE_SORT` bit into `dict_mem_index_add_field()`.
- `mariadb/storage/innobase/handler/handler0alter.cc` records descending
  key-part metadata in in-place alter structures, including index-field
  definitions used for secondary-index DDL.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes unique indexes
  with `NON_UNIQUE = 0` and per-key-part direction through
  `information_schema.statistics.COLLATION`.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for standalone
  `CREATE UNIQUE INDEX ... (tenant_id, score DESC)` and `DROP INDEX`.
- Verify an already-open ownerless peer observes `NON_UNIQUE = 0`,
  `COLLATION = 'A'` for sequence 1, and `COLLATION = 'D'` for sequence 2.
- Verify duplicate-key rejection while the unique descending index exists.
- Verify dropping the index refreshes the already-open peer, makes
  `FORCE INDEX` fail, permits the formerly duplicate row shape, and leaves the
  table readable.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- TEXT/BLOB prefix unique indexes, full algorithm/lock option matrix, crash
  recovery during unique descending-index DDL, and external randomized DDL
  oracles. Descending primary-key replacement is covered separately by
  `ownerless-descending-primary-key-ddl-refresh`, composite direction
  primary-key replacement is covered separately by
  `ownerless-composite-direction-primary-key-ddl-refresh`, and unique prefix
  secondary-index DDL is covered separately by
  `ownerless-unique-prefix-index-ddl-refresh`.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `unique-descending-index-ddl`:

1. A child ownerless process creates
   `app.ownerless_unique_descending_index_base` with `id`, `tenant_id`,
   `score`, and `weight` columns and three rows.
2. The child creates `ownerless_unique_desc_tenant_score` as a standalone
   unique secondary index over `tenant_id, score DESC`.
3. The already-open ownerless parent observes statistics rows with
   `NON_UNIQUE = 0`, sequence 1 `COLLATION = 'A'`, and sequence 2
   `COLLATION = 'D'`, uses `FORCE INDEX`, verifies duplicate-key rejection,
   inserts a distinct row, and verifies the forced-index aggregate includes it.
4. The child drops the index.
5. The parent observes index absence, verifies `FORCE INDEX` fails, inserts a
   row that would have duplicated the formerly indexed key, and checks the
   base table remains readable.
6. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if ownerless dictionary
generation, metadata flush, and existing InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless index DDL coverage to a representative unique
descending secondary-index option. It does not claim primary-key, prefix, or
online-option matrix coverage for unique descending indexes.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
secondary-index DDL inside the MyLite database directory. The final state is
verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native unique secondary-index enforcement plus descending key-part metadata.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `unique-descending-index-ddl` plus adjacent `unique-index-ddl`,
  `descending-index-ddl`, `mixed-direction-index-ddl`, `prefix-index-ddl`, and
  `index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks, using focused reruns if the known intermittent InnoDB log-header
  checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `NON_UNIQUE = 0` plus `A`/`D`
  key-part direction metadata for the unique descending index.
- Duplicate-key insertion fails while the unique descending index exists.
- The peer can use the unique descending index with `FORCE INDEX` while it
  exists.
- After peer `DROP INDEX`, the already-open peer observes index absence,
  forced-index use fails, a formerly duplicate row shape can be inserted, and
  table data remains readable.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- TEXT/BLOB prefix unique indexes, algorithm/lock matrices, and crash recovery
  during index DDL remain planned. Descending primary-key replacement is
  covered separately by `ownerless-descending-primary-key-ddl-refresh`,
  composite direction primary-key replacement is covered separately by
  `ownerless-composite-direction-primary-key-ddl-refresh`, and unique prefix
  secondary-index DDL is covered separately by
  `ownerless-unique-prefix-index-ddl-refresh`.
- External randomized DDL/RQG stress remains separate validation work.
