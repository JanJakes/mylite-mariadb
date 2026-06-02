# Ownerless Unique TEXT/BLOB Prefix Index DDL Refresh

## Problem

Ownerless TEXT/BLOB prefix-index coverage proves shortened key-part metadata
for non-unique long-value indexes, and unique-prefix coverage proves
uniqueness enforcement for a bounded `VARCHAR` prefix. Unique TEXT/BLOB prefix
indexes combine both behaviors on MariaDB's long-value key path: the key part
must carry an explicit prefix length, the index is unique, and InnoDB enforces
uniqueness over the stored prefix bytes.

MyLite needs bounded ownerless evidence that unique TEXT and BLOB prefix
InnoDB indexes created by one ownerless process refresh already-open peers,
expose `NON_UNIQUE = 0` plus `SUB_PART`, reject duplicate prefixes while
present, disappear after `DROP INDEX`, and survive ownerless/native reopen
before and after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:prepare_key_column()` derives key-part length,
  including explicit prefix lengths for long value fields, caps them against
  handler key limits, and stores the result in the parsed key part.
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` copies the
  parsed key-part length into `KEY_PART_INFO::length`, marks partial key
  segments with `HA_KEY_HAS_PART_KEY_SEG`, and records uniqueness through
  `KEY::UNIQUE`/`HA_NOSAME`.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes unique-index
  metadata as `NON_UNIQUE = 0` and prefix metadata through
  `information_schema.statistics.SUB_PART`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` handles
  BLOB-family key parts, compares key-part length with the table field length,
  passes the resulting `prefix_len` into `dict_mem_index_add_field()`, and
  creates unique secondary indexes from MariaDB `KEY` metadata.
- `mariadb/storage/innobase/handler/handler0alter.cc` records prefix lengths
  for in-place alter index-field definitions, including BLOB-family fields.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for standalone TEXT and BLOB prefix
  `CREATE UNIQUE INDEX` plus `DROP INDEX`.
- Verify an already-open ownerless peer observes `NON_UNIQUE = 0`,
  `SUB_PART = 5` for the TEXT key part, and `SUB_PART = 4` for the BLOB key
  part.
- Verify duplicate TEXT-prefix and BLOB-prefix rejection while the indexes
  exist.
- Verify forced-index reads and ordinary writes work while both unique prefix
  indexes exist.
- Verify dropping both indexes refreshes the already-open peer, makes
  `FORCE INDEX` fail for both index names, permits formerly duplicate prefixes,
  and leaves the table readable.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- Unique TEXT/BLOB prefix-plus-direction combinations, charset-width edge
  cases, full algorithm/lock option matrix, crash recovery during unique
  TEXT/BLOB prefix-index DDL, and external randomized DDL oracles. Non-unique
  TEXT/BLOB prefix-plus-direction indexes are covered separately by
  `ownerless-text-blob-prefix-direction-index-ddl-refresh`.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.

## Design

Add a selector named `unique-text-blob-prefix-index-ddl`:

1. A child ownerless process creates
   `app.ownerless_unique_text_blob_prefix_index_base` with `id`,
   latin1 `body TEXT`, `payload BLOB`, and `weight` columns plus three rows
   whose TEXT five-character prefixes and BLOB four-byte prefixes are
   distinct.
2. The child creates `ownerless_unique_text_prefix_body_idx` over `body(5)` and
   `ownerless_unique_blob_prefix_payload_idx` over `payload(4)`.
3. The already-open ownerless parent observes statistics rows with
   `NON_UNIQUE = 0`, `SUB_PART = 5`, and `SUB_PART = 4`, uses both indexes
   with `FORCE INDEX`, verifies duplicate TEXT-prefix and BLOB-prefix
   rejection, inserts a distinct-prefix row, and verifies aggregate results.
4. The child drops both indexes.
5. The parent observes both indexes absent, verifies forced-index use fails for
   both names, inserts a row that would have duplicated both indexed prefixes,
   and checks the base table remains readable.
6. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if ownerless dictionary
generation, metadata flush, and existing InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless index DDL coverage to representative unique TEXT and
BLOB prefix secondary-index options. It does not claim unique TEXT/BLOB
prefix-plus-direction, charset-width, or online-option matrix coverage.
Non-unique TEXT/BLOB prefix-plus-direction indexes are covered by a separate
focused slice.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
secondary-index DDL inside the MyLite database directory. The final state is
verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native unique TEXT and BLOB prefix secondary-index metadata and enforcement
path.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `unique-text-blob-prefix-index-ddl` plus adjacent
  `text-blob-prefix-index-ddl`, `unique-prefix-index-ddl`,
  `unique-prefix-direction-index-ddl`, `prefix-index-ddl`, and
  `unique-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks, using focused reruns if the known intermittent InnoDB log-header
  checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `NON_UNIQUE = 0`, `SUB_PART = 5` for
  the TEXT prefix index, and `SUB_PART = 4` for the BLOB prefix index.
- Duplicate TEXT-prefix and BLOB-prefix inserts fail while the unique prefix
  indexes exist.
- The peer can use both prefix indexes with `FORCE INDEX` while they exist.
- After peer `DROP INDEX`, the already-open peer observes both indexes absent,
  forced-index use fails for both names, formerly duplicate prefixes can be
  inserted, and table data remains readable.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- Unique TEXT/BLOB prefix-plus-direction combinations, charset-width edge
  cases, algorithm/lock matrices, and crash recovery during index DDL remain
  planned. Non-unique TEXT/BLOB prefix-plus-direction indexes are covered
  separately by `ownerless-text-blob-prefix-direction-index-ddl-refresh`.
- External randomized DDL/RQG stress remains separate validation work.
