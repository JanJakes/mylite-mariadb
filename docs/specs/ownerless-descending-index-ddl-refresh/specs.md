# Ownerless Descending Index DDL Refresh

## Problem

Ownerless standalone index coverage proves ordinary secondary-index
create/use/drop refresh, and unique-index coverage proves multi-column
uniqueness enforcement. Descending index parts remain a separate index-option
class: MariaDB records the direction in key-part metadata, exposes it through
`information_schema.statistics.COLLATION`, and passes it into InnoDB native
index definitions.

MyLite needs bounded ownerless evidence that a descending InnoDB index created
by one ownerless process refreshes already-open peers, remains usable for
forced-index reads and writes, disappears after `DROP INDEX`, and survives
ownerless/native reopen before and after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/include/my_base.h` defines `HA_REVERSE_SORT` as the key-part flag
  for reverse sort order.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores
  `HA_REVERSE_SORT` in `KEY_PART_INFO::key_part_flag` when an index column is
  declared descending; non-B-tree special indexes ignore the direction.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` passes
  `HA_REVERSE_SORT` into `dict_mem_index_add_field()` for ordinary InnoDB
  secondary indexes.
- `mariadb/storage/innobase/handler/handler0alter.cc` records descending
  key-part metadata in in-place alter structures and treats ASC/DESC changes
  on primary-key columns as order-changing DDL.
- `mariadb/sql/sql_show.cc` emits `DESC` in `SHOW CREATE TABLE` for reverse
  ordered key parts and exposes `information_schema.statistics.COLLATION` as
  `D` for descending and `A` for ascending key parts.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for standalone `CREATE INDEX ... (value
  DESC)` and `DROP INDEX`.
- Verify an already-open ownerless peer observes the descending key part via
  `information_schema.statistics.COLLATION = 'D'`.
- Verify forced-index reads and ordinary writes work while the descending
  index exists.
- Verify dropping the index refreshes the already-open peer, makes
  `FORCE INDEX` fail, and leaves the table readable.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- Full algorithm/lock option matrix, crash recovery during descending-index
  DDL, and external randomized DDL oracles. Descending primary-key replacement
  is covered separately by
  `ownerless-descending-primary-key-ddl-refresh`, unique descending
  secondary-index DDL is covered separately by
  `ownerless-unique-descending-index-ddl-refresh`, composite mixed-direction
  secondary-index DDL is covered separately by
  `ownerless-mixed-direction-index-ddl-refresh`, and prefix secondary-index DDL
  is covered separately by `ownerless-prefix-index-ddl-refresh`.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `descending-index-ddl`:

1. A child ownerless process creates `app.ownerless_descending_index_base` with
   `id`, `value`, and `note` columns and three rows.
2. The child creates `ownerless_desc_value_idx` as a standalone descending
   secondary index over `value DESC`.
3. The already-open ownerless parent observes one statistics row with
   `COLLATION = 'D'`, uses `FORCE INDEX`, inserts another row, and verifies the
   forced-index aggregate includes it.
4. The child drops the index.
5. The parent observes index absence, verifies `FORCE INDEX` fails, and checks
   the base table remains readable.
6. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if ownerless dictionary
generation, metadata flush, and existing InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless index DDL coverage to a representative descending
secondary-index option. It does not claim broad mixed-direction, primary-key,
unique, or online-option matrix coverage.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
secondary-index DDL inside the MyLite database directory. The final state is
verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native descending secondary-index metadata and storage path.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `descending-index-ddl` plus adjacent `index-ddl`,
  `unique-index-ddl`, `rename-index-ddl`, and `ignored-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage, hook ownerless SQL
  coverage, ownerless stress, `format-check`, `git diff --check`, and cached
  diff checks, using focused reruns if the known intermittent InnoDB
  log-header checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `COLLATION = 'D'` for the descending
  index key part.
- The peer can use the descending index with `FORCE INDEX` while it exists.
- After peer `DROP INDEX`, the already-open peer observes index absence,
  forced-index use fails, and table data remains readable.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.
- Compatibility docs distinguish this bounded descending-index evidence from
  broader index option and external-oracle stress gaps.

## Risks And Follow-Up

- Algorithm/lock option combinations and crash recovery during index DDL remain
  planned. Descending primary-key replacement is covered separately by
  `ownerless-descending-primary-key-ddl-refresh`, unique descending
  secondary-index DDL is covered separately by
  `ownerless-unique-descending-index-ddl-refresh`, composite mixed-direction
  secondary-index DDL is covered separately by
  `ownerless-mixed-direction-index-ddl-refresh`, and prefix secondary-index DDL
  is covered separately by `ownerless-prefix-index-ddl-refresh`.
- External randomized DDL/RQG stress remains separate validation work.
