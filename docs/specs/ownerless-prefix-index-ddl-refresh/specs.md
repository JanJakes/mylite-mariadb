# Ownerless Prefix Index DDL Refresh

## Problem

Ownerless standalone index coverage proves ordinary secondary-index
create/use/drop refresh, unique-index coverage proves uniqueness enforcement,
and descending-index coverage proves key-part direction metadata. Prefix
indexes remain a separate key-part metadata class: MariaDB stores a shortened
key-part length, exposes it through `information_schema.statistics.SUB_PART`,
and passes the prefix length into InnoDB native index definitions.

MyLite needs bounded ownerless evidence that a prefix InnoDB index created by
one ownerless process refreshes already-open peers, remains usable for
forced-index reads and writes, disappears after `DROP INDEX`, and survives
ownerless/native reopen before and after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores the
  parsed prefix length in `KEY_PART_INFO::length` and marks partial key
  segments with `HA_KEY_HAS_PART_KEY_SEG`.
- `mariadb/sql/sql_show.cc:store_key_options()` emits the prefix length in
  `SHOW CREATE TABLE` when the key-part length is shorter than the field key
  length.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes prefix metadata
  through `information_schema.statistics.SUB_PART` when the stored key-part
  length differs from the full field key length.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` compares the
  key-part length with the table field length and passes the resulting
  `prefix_len` into `dict_mem_index_add_field()`.
- `mariadb/storage/innobase/handler/handler0alter.cc` records the same prefix
  length in InnoDB in-place alter index-field definitions.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for standalone `CREATE INDEX ... (code(4))`
  and `DROP INDEX`.
- Verify an already-open ownerless peer observes the prefix key part via
  `information_schema.statistics.SUB_PART = 4`.
- Verify forced-index reads and ordinary writes work while the prefix index
  exists.
- Verify dropping the index refreshes the already-open peer, makes
  `FORCE INDEX` fail, and leaves the table readable.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- Prefix unique indexes, TEXT/BLOB prefix indexes, mixed prefix plus
  descending composite indexes, charset-width edge cases, full algorithm/lock
  option matrix, crash recovery during prefix-index DDL, and external
  randomized DDL oracles.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `prefix-index-ddl`:

1. A child ownerless process creates `app.ownerless_prefix_index_base` with
   `id`, `code`, and `weight` columns and three rows.
2. The child creates `ownerless_prefix_code_idx` as a standalone prefix
   secondary index over `code(4)`.
3. The already-open ownerless parent observes one statistics row with
   `SUB_PART = 4`, uses `FORCE INDEX`, inserts another row, and verifies the
   forced-index aggregate includes it.
4. The child drops the index.
5. The parent observes index absence, verifies `FORCE INDEX` fails, and checks
   the base table remains readable.
6. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if ownerless dictionary
generation, metadata flush, and existing InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless index DDL coverage to a representative prefix
secondary-index option. It does not claim broad charset-width, TEXT/BLOB,
unique-prefix, composite, or online-option matrix coverage.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
secondary-index DDL inside the MyLite database directory. The final state is
verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native prefix secondary-index metadata and storage path.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `prefix-index-ddl` plus adjacent `index-ddl`,
  `unique-index-ddl`, `descending-index-ddl`, `rename-index-ddl`, and
  `ignored-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run embedded ownerless cross-process SQL coverage, hook ownerless SQL
  coverage, ownerless stress, `format-check`, `git diff --check`, and cached
  diff checks, using focused reruns if the known intermittent InnoDB
  log-header checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `SUB_PART = 4` for the prefix index
  key part.
- The peer can use the prefix index with `FORCE INDEX` while it exists.
- After peer `DROP INDEX`, the already-open peer observes index absence,
  forced-index use fails, and table data remains readable.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.
- Compatibility docs distinguish this bounded prefix-index evidence from
  broader index option and external-oracle stress gaps.

## Risks And Follow-Up

- Prefix unique indexes, TEXT/BLOB prefix indexes, mixed prefix plus
  descending composite indexes, charset-width edge cases, algorithm/lock
  matrices, and crash recovery during index DDL remain planned.
- External randomized DDL/RQG stress remains separate validation work.
