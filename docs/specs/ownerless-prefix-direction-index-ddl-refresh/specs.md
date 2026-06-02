# Ownerless Prefix Direction Index DDL Refresh

## Problem

Ownerless prefix-index coverage proves shortened key-part metadata, and
mixed-direction coverage proves per-key-part `ASC`/`DESC` metadata. A
descending prefixed key part combines both: MariaDB stores a shortened
key-part length and a reverse-sort flag for the same key part, exposes both
through `information_schema.statistics`, and passes both into InnoDB native
index definitions.

MyLite needs bounded ownerless evidence that a prefixed descending InnoDB
index key part created by one ownerless process refreshes already-open peers,
remains usable for forced-index reads and writes, disappears after
`DROP INDEX`, and survives ownerless/native reopen before and after forced
shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores the
  parsed prefix length in `KEY_PART_INFO::length`, marks partial key segments
  with `HA_KEY_HAS_PART_KEY_SEG`, and stores `HA_REVERSE_SORT` when an index
  column is declared descending.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes prefix metadata
  through `information_schema.statistics.SUB_PART` and key-part direction
  through `information_schema.statistics.COLLATION`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` compares the
  key-part length with the table field length, passes the resulting
  `prefix_len` into `dict_mem_index_add_field()`, and also passes the key
  part's `HA_REVERSE_SORT` bit.
- `mariadb/storage/innobase/handler/handler0alter.cc` records both
  `prefix_len` and descending metadata in InnoDB in-place alter index-field
  definitions.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for standalone
  `CREATE INDEX ... (code(4) DESC, score ASC)` and `DROP INDEX`.
- Verify an already-open ownerless peer observes `SUB_PART = 4` and
  `COLLATION = 'D'` on the prefixed key part.
- Verify the same peer observes `COLLATION = 'A'` on the second key part.
- Verify forced-index reads and ordinary writes work while the index exists.
- Verify dropping the index refreshes the already-open peer, makes
  `FORCE INDEX` fail, and leaves the table readable.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- Charset-width edge cases, full algorithm/lock option matrix, crash recovery
  during prefix-direction index DDL, and external randomized DDL oracles.
  TEXT/BLOB prefix-plus-direction indexes are covered separately by
  `ownerless-text-blob-prefix-direction-index-ddl-refresh`. Unique
  prefix-plus-direction secondary-index DDL is covered separately by
  `ownerless-unique-prefix-direction-index-ddl-refresh`, composite direction
  primary-key replacement is covered separately by
  `ownerless-composite-direction-primary-key-ddl-refresh`, and descending
  primary-key replacement is covered separately by
  `ownerless-descending-primary-key-ddl-refresh`.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `prefix-direction-index-ddl`:

1. A child ownerless process creates
   `app.ownerless_prefix_direction_index_base` with `id`, latin1 `code`,
   `score`, and `weight` columns plus three rows.
2. The child creates `ownerless_prefix_direction_idx` over
   `code(4) DESC, score ASC`.
3. The already-open ownerless parent observes statistics rows for sequence 1
   with `SUB_PART = 4` and `COLLATION = 'D'`, and sequence 2 with
   `COLLATION = 'A'`, uses `FORCE INDEX`, inserts another row, and verifies
   aggregate results.
4. The child drops the index.
5. The parent observes index absence, verifies forced-index use fails, inserts
   another matching row, and checks the base table remains readable.
6. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if ownerless dictionary
generation, metadata flush, and existing InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless index DDL coverage to a representative prefixed
descending key-part option on an ordinary `VARCHAR` secondary index. It does
not claim charset-width, primary-key, or online-option matrix coverage for
prefix-plus-direction indexes; TEXT/BLOB prefix-plus-direction and unique
prefix-plus-direction DDL are covered by separate focused slices.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
secondary-index DDL inside the MyLite database directory. The final state is
verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native prefix-length plus descending-key-part metadata path for an ordinary
secondary index.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `prefix-direction-index-ddl` plus adjacent `prefix-index-ddl`,
  `mixed-direction-index-ddl`, `descending-index-ddl`,
  `unique-prefix-index-ddl`, and `text-blob-prefix-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks, using focused reruns if the known intermittent InnoDB log-header
  checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `SUB_PART = 4` and
  `COLLATION = 'D'` for the prefixed descending key part.
- The peer observes `COLLATION = 'A'` for the second key part.
- The peer can use the index with `FORCE INDEX` while it exists.
- After peer `DROP INDEX`, the already-open peer observes index absence,
  forced-index use fails, and table data remains readable.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- Charset-width edge cases, algorithm/lock matrices, and crash recovery during
  index DDL remain planned. TEXT/BLOB prefix-plus-direction indexes are covered
  separately by `ownerless-text-blob-prefix-direction-index-ddl-refresh`.
  Unique prefix-plus-direction secondary-index DDL is covered separately by
  `ownerless-unique-prefix-direction-index-ddl-refresh`, composite direction
  primary-key replacement is covered separately by
  `ownerless-composite-direction-primary-key-ddl-refresh`, and descending
  primary-key replacement is covered separately by
  `ownerless-descending-primary-key-ddl-refresh`.
- External randomized DDL/RQG stress remains separate validation work.
