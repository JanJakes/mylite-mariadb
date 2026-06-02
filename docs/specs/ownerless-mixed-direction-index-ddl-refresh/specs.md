# Ownerless Mixed-Direction Index DDL Refresh

## Problem

Ownerless descending-index coverage proves one reverse-ordered key part, and
prefix-index coverage proves shortened key-part metadata. Composite indexes
with mixed ascending and descending parts remain a separate metadata shape:
each `KEY_PART_INFO` entry carries its own direction flag, and
`information_schema.statistics` exposes direction per sequence position.

MyLite needs bounded ownerless evidence that a composite InnoDB index with
mixed `ASC`/`DESC` key parts created by one ownerless process refreshes
already-open peers, remains usable for forced-index reads and writes,
disappears after `DROP INDEX`, and survives ownerless/native reopen before and
after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/include/my_base.h` defines `HA_REVERSE_SORT` as the key-part flag
  for reverse sort order.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores
  `HA_REVERSE_SORT` independently on each `KEY_PART_INFO` when the
  corresponding index column is declared descending.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` passes each
  key part's `HA_REVERSE_SORT` bit into `dict_mem_index_add_field()`.
- `mariadb/storage/innobase/handler/handler0alter.cc` records each key part's
  descending attribute in InnoDB in-place alter index-field definitions.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes
  `information_schema.statistics.COLLATION` as `A` or `D` for each key-part
  sequence position.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for standalone
  `CREATE INDEX ... (tenant_id ASC, score DESC)` and `DROP INDEX`.
- Verify an already-open ownerless peer observes `COLLATION = 'A'` for
  sequence 1 and `COLLATION = 'D'` for sequence 2.
- Verify forced-index reads and ordinary writes work while the mixed-direction
  index exists.
- Verify dropping the index refreshes the already-open peer, makes
  `FORCE INDEX` fail, and leaves the table readable.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- Unique mixed-direction indexes, primary-key direction changes, full
  algorithm/lock option matrix, crash recovery during mixed-direction index
  DDL, and external randomized DDL oracles. Prefix plus direction secondary
  index DDL is covered separately by
  `ownerless-prefix-direction-index-ddl-refresh`.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `mixed-direction-index-ddl`:

1. A child ownerless process creates `app.ownerless_mixed_direction_index_base`
   with `id`, `tenant_id`, `score`, and `weight` columns and three rows.
2. The child creates `ownerless_mixed_direction_idx` as a standalone composite
   secondary index over `tenant_id ASC, score DESC`.
3. The already-open ownerless parent observes statistics rows for sequence 1
   with `COLLATION = 'A'` and sequence 2 with `COLLATION = 'D'`, uses
   `FORCE INDEX`, inserts another row, and verifies the forced-index aggregate
   includes it.
4. The child drops the index.
5. The parent observes index absence, verifies `FORCE INDEX` fails, and checks
   the base table remains readable.
6. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if ownerless dictionary
generation, metadata flush, and existing InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless index DDL coverage to a representative composite
mixed-direction secondary-index option. It does not claim unique, primary-key,
prefix-plus-direction, or online-option matrix coverage for mixed directions;
prefix plus direction secondary-index DDL is covered by a separate focused
slice.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
secondary-index DDL inside the MyLite database directory. The final state is
verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native per-key-part direction metadata and storage path for a composite
secondary index.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `mixed-direction-index-ddl` plus adjacent `index-ddl`,
  `unique-index-ddl`, `descending-index-ddl`, `prefix-index-ddl`,
  `rename-index-ddl`, and `ignored-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded and hook all-selector coverage, ownerless stress,
  `format-check`, `git diff --check`, and cached diff checks, using focused
  reruns if the known intermittent InnoDB log-header checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `COLLATION = 'A'` for the first key
  part and `COLLATION = 'D'` for the second key part.
- The peer can use the mixed-direction index with `FORCE INDEX` while it
  exists.
- After peer `DROP INDEX`, the already-open peer observes index absence,
  forced-index use fails, and table data remains readable.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.
- Compatibility docs distinguish this bounded mixed-direction evidence from
  broader index option and external-oracle stress gaps.

## Risks And Follow-Up

- Unique mixed-direction indexes, primary-key direction changes,
  algorithm/lock matrices, and crash recovery during index DDL remain planned.
  Prefix plus direction secondary-index DDL is covered separately by
  `ownerless-prefix-direction-index-ddl-refresh`.
- External randomized DDL/RQG stress remains separate validation work.
