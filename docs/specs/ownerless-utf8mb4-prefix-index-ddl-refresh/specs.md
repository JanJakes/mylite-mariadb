# Ownerless utf8mb4 Prefix Index DDL Refresh

## Problem

Ownerless prefix-index coverage proves shortened key-part metadata for
single-byte `VARCHAR` values, and unique prefix coverage proves duplicate
prefix enforcement. Multibyte character sets add another metadata boundary:
MariaDB stores the key-part prefix length in bytes for the engine, while
`information_schema.statistics.SUB_PART` exposes the user-declared character
prefix length.

MyLite needs bounded ownerless evidence that a unique `utf8mb4` prefix InnoDB
index created by one ownerless process refreshes already-open peers, exposes the
character-count `SUB_PART`, enforces duplicate first-character prefixes while
present, disappears after `DROP INDEX`, and survives ownerless/native reopen
before and after forced shared-memory rebuild.

## Source Findings

- Base: MariaDB `mariadb-11.8.6`
  (`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).
- `mariadb/sql/sql_table.cc:mysql_prepare_create_table_finalize()` stores the
  parsed key-part length in `KEY_PART_INFO::length`, marks partial key
  segments with `HA_KEY_HAS_PART_KEY_SEG`, and records uniqueness through
  `KEY::UNIQUE`/`HA_NOSAME`.
- `mariadb/sql/sql_show.cc:get_schema_stat_record()` exposes
  `information_schema.statistics.SUB_PART` by dividing the stored key-part
  length by the field character set's `mbmaxlen`, so a one-character
  `utf8mb4` prefix is reported as `SUB_PART = 1`.
- `mariadb/storage/innobase/handler/ha_innodb.cc:create_index()` derives
  `prefix_len` from `KEY_PART_INFO::length`, asserts it is aligned to
  `field->charset()->mbmaxlen`, records `DICT_UNIQUE` for `HA_NOSAME`, and
  passes the byte prefix into `dict_mem_index_add_field()`.
- `mariadb/storage/innobase/handler/handler0alter.cc`
  `innobase_create_index_field_def()` records the same prefix length in InnoDB
  in-place alter index-field definitions.

## Scope And Non-Goals

In scope:

- Add a focused ownerless selector for standalone
  `CREATE UNIQUE INDEX ... (code(1))` and `DROP INDEX` over a `utf8mb4`
  `VARCHAR` column whose first character is encoded as four bytes.
- Verify an already-open ownerless peer observes `NON_UNIQUE = 0` and
  `SUB_PART = 1`.
- Verify `FORCE INDEX` reads work while the native unique prefix index exists.
- Verify duplicate first-character prefix rejection while the unique prefix
  index exists.
- Verify dropping the index refreshes the already-open peer, makes
  `FORCE INDEX` fail, permits the formerly duplicate first-character prefix,
  and leaves the table readable.
- Verify final rows and absent-index metadata through ownerless/native reopen
  before and after forced `.shm` rebuild.

Out of scope:

- Broad charset-width matrix coverage across collations, prefix lengths, TEXT
  families, binary string types, online DDL options, and crash recovery during
  prefix-index DDL.
- SQL-level table-lock fault injection; prior exploratory SQL shapes did not
  reach the ownerless table-wait callback.
- External randomized DDL/RQG stress.

## Design

Add a selector named `utf8mb4-prefix-index-ddl`:

1. A child ownerless process creates
   `app.ownerless_utf8mb4_prefix_index_base` with `id`, `code`, and `weight`
   columns and three rows whose first `utf8mb4` characters are distinct.
   The SQL uses `CONVERT(X'...' USING utf8mb4)` to keep the C source ASCII.
2. The child creates `ownerless_utf8mb4_prefix_code_idx` as a standalone
   unique prefix secondary index over `code(1)`.
3. The already-open ownerless parent observes `NON_UNIQUE = 0` and
   `SUB_PART = 1`, uses `FORCE INDEX`, verifies duplicate first-character
   prefix rejection, inserts a distinct-prefix row, and verifies aggregate
   results.
4. The child drops the index.
5. The parent observes index absence, verifies `FORCE INDEX` fails, inserts a
   row that would have duplicated the indexed prefix, and checks the base table
   remains readable.
6. Helper assertions verify final rows and absent-index metadata through
   ownerless/native reopen before and after forced shared-memory rebuild.

The slice should require no product-code change if ownerless dictionary
generation, metadata flush, and existing InnoDB DDL publication are correct.

## Compatibility Impact

This extends ownerless index DDL coverage to a representative multibyte
character-set prefix option. It does not claim complete charset-width,
collation, long-value, online-option, crash-recovery, or external-oracle
coverage.

## Directory And Lifecycle Impact

No new durable files or layout changes. MariaDB/InnoDB owns the native
secondary-index DDL inside the MyLite database directory. The final state is
verified after normal ownerless/native reopen and forced volatile
shared-memory rebuild.

## Native Storage Impact

Native InnoDB storage format is unchanged. The selector exercises MariaDB's
native byte-prefix storage path for a user-visible multibyte character prefix.

## Public API Impact

No public API changes.

## Binary Size Impact

No production binary-size impact beyond focused test code and docs.

## Test Plan

- Build `mylite_ownerless_cross_process_sql_test` in `embedded-dev`.
- Run focused `utf8mb4-prefix-index-ddl` plus adjacent `unique-prefix-index-ddl`,
  `prefix-index-ddl`, `unique-text-blob-prefix-index-ddl`,
  `unique-text-blob-prefix-direction-index-ddl`, and
  `text-blob-prefix-index-ddl` selectors.
- Build and run the focused selector in `ownerless-test-hooks`.
- Run ownerless stress, `format-check`, `git diff --check`, and cached diff
  checks, using focused reruns if the known intermittent InnoDB log-header
  checksum abort appears.

## Acceptance Criteria

- An already-open ownerless peer observes `NON_UNIQUE = 0` and `SUB_PART = 1`
  for the `utf8mb4` unique prefix index.
- Duplicate first-character prefix insertion fails while the unique prefix
  index exists.
- The peer can use the unique prefix index with `FORCE INDEX` while it exists.
- After peer `DROP INDEX`, the already-open peer observes index absence,
  forced-index use fails, a formerly duplicate prefix can be inserted, and
  table data remains readable.
- Final rows and absent-index state survive ownerless/native reopen before and
  after forced `.shm` rebuild.

## Risks And Follow-Up

- Broader charset-width, collation, TEXT/BLOB, binary-string, algorithm/lock
  option, and crash-recovery matrices for prefix-index DDL remain planned.
- External randomized DDL/RQG stress remains separate validation work.
