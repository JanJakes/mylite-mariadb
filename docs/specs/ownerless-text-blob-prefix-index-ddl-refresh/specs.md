# Ownerless TEXT/BLOB Prefix Index DDL Refresh

## Problem

Ownerless prefix-index coverage proves shortened key-part metadata for a
bounded `VARCHAR` column. TEXT and BLOB prefix indexes remain a separate native
index class because MariaDB requires a prefix length for those long values,
stores that shortened length in key-part metadata, exposes it through
`information_schema.statistics.SUB_PART`, and passes it to InnoDB as a native
prefix index field.

MyLite needs bounded ownerless evidence that TEXT and BLOB prefix InnoDB
indexes created by one ownerless process refresh already-open peers, remain
usable for forced-index reads and writes, disappear after `DROP INDEX`, and
survive ownerless/native reopen before and after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:prepare_key_column()` derives key-part length,
  including explicit prefix lengths, caps them against handler key limits, and
  stores the result in the parsed key part.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` copies the
  parsed key-part length into `KEY_PART_INFO::length` and marks partial key
  segments with `HA_KEY_HAS_PART_KEY_SEG`.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes prefix metadata
  through `information_schema.statistics.SUB_PART` when the stored key-part
  length differs from the full field key length.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` handles
  `MYSQL_TYPE_BLOB`, compares key-part length with the table field length, and
  passes the resulting `prefix_len` into `dict_mem_index_add_field()`.
- `mariadb/storage/innobase/handler/handler0alter.cc` records prefix lengths
  for in-place alter index-field definitions, including BLOB-family fields.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for standalone TEXT and BLOB prefix
  secondary-index `CREATE INDEX` plus `DROP INDEX`.
- Verify an already-open ownerless peer observes `SUB_PART = 5` for the TEXT
  key part and `SUB_PART = 4` for the BLOB key part.
- Verify forced-index reads and ordinary writes work while both prefix indexes
  exist.
- Verify dropping both indexes refreshes the already-open peer, makes
  `FORCE INDEX` fail for both index names, and leaves the table readable.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- Charset-width edge cases, full algorithm/lock option matrix, crash recovery
  during TEXT/BLOB prefix-index DDL, and external randomized DDL oracles.
  TEXT/BLOB prefix-plus-direction indexes are covered separately by
  `ownerless-text-blob-prefix-direction-index-ddl-refresh`, and unique
  TEXT/BLOB prefix indexes are covered separately by
  `ownerless-unique-text-blob-prefix-index-ddl-refresh`.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `text-blob-prefix-index-ddl`:

1. A child ownerless process creates
   `app.ownerless_text_blob_prefix_index_base` with `id`, latin1 `body TEXT`,
   `payload BLOB`, and `weight` columns plus three rows.
2. The child creates `ownerless_text_prefix_body_idx` over `body(5)` and
   `ownerless_blob_prefix_payload_idx` over `payload(4)`.
3. The already-open ownerless parent observes statistics rows with
   `SUB_PART = 5` and `SUB_PART = 4`, uses both indexes with `FORCE INDEX`,
   inserts another matching row, and verifies aggregate results.
4. The child drops both indexes.
5. The parent observes both indexes absent, verifies forced-index use fails for
   both names, inserts another matching row, and checks the base table remains
   readable.
6. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if ownerless dictionary
generation, metadata flush, and existing InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless index DDL coverage to representative TEXT and BLOB
prefix secondary-index options. It does not claim charset-width, direction, or
online-option matrix coverage. Unique TEXT/BLOB prefix indexes are covered by a
separate focused slice, and TEXT/BLOB prefix-plus-direction indexes are covered
by a separate focused slice.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
secondary-index DDL inside the MyLite database directory. The final state is
verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native TEXT and BLOB prefix secondary-index metadata and storage path.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `text-blob-prefix-index-ddl` plus adjacent `prefix-index-ddl`,
  `unique-prefix-index-ddl`, `index-ddl`, `descending-index-ddl`, and
  `rename-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks, using focused reruns if the known intermittent InnoDB log-header
  checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `SUB_PART = 5` for the TEXT prefix
  index and `SUB_PART = 4` for the BLOB prefix index.
- The peer can use both prefix indexes with `FORCE INDEX` while they exist.
- Writes remain valid while the indexes exist.
- After peer `DROP INDEX`, the already-open peer observes both indexes absent,
  forced-index use fails for both names, and table data remains readable.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- Charset-width edge cases, algorithm/lock matrices, and crash recovery during
  index DDL remain planned. TEXT/BLOB prefix-plus-direction indexes are covered
  separately by `ownerless-text-blob-prefix-direction-index-ddl-refresh`, and
  unique TEXT/BLOB prefix indexes are covered separately by
  `ownerless-unique-text-blob-prefix-index-ddl-refresh`.
- External randomized DDL/RQG stress remains separate validation work.
