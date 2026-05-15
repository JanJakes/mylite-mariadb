# CREATE OR REPLACE TABLE

## Problem

MyLite supports routed base-table creation, `CREATE TABLE ... LIKE`, CTAS, and
drop lifecycle, but `CREATE OR REPLACE TABLE` is still outside the current
support claim. Import tools and migrations use OR REPLACE to refresh staging
tables without a separate client-side drop step. MyLite needs representative
coverage that successful OR REPLACE flows replace the old catalog record, drop
old rows and indexes from SQL visibility, publish the new routed definition,
and survive close/reopen without MariaDB sidecars.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/structs.h:591-621` defines `DDL_options_st::OPT_OR_REPLACE`
  and exposes `DDL_options_st::or_replace()`.
- `mariadb/sql/structs.h:606-609` keeps `OPT_OR_REPLACE` when building
  `CREATE TABLE ... LIKE` create-info through `create_like_options()`.
- `mariadb/sql/sql_table.cc:4714-4741` handles OR REPLACE over an existing
  temporary table by dropping the old temporary table before re-creating it.
- `mariadb/sql/sql_table.cc:4772-4822` handles OR REPLACE over an existing
  base table by calling `mysql_rm_table_no_locks()` before the new create and
  marking `table_was_deleted`.
- `mariadb/sql/sql_table.cc:mysql_create_like_table()` opens the LIKE source,
  rejects target/source duplication for non-temporary OR REPLACE, builds the
  cloned definition, and creates the replacement through the normal no-lock
  create path.
- `mariadb/sql/sql_insert.cc:select_create::create_table_from_items()` uses
  the same no-lock create path for CTAS targets, so OR REPLACE CTAS should
  reuse MariaDB's existing drop-then-create flow before row population.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::delete_table()` maps the
  MariaDB drop step to `mylite_storage_drop_table()`, while
  `ha_mylite::create()` publishes the replacement definition through
  `mylite_storage_store_table_definition()`.

## Scope

- Successful `CREATE OR REPLACE TABLE target LIKE source` for supported
  MyLite-routed source and target tables.
- Successful `CREATE OR REPLACE TABLE target ENGINE=InnoDB AS SELECT ...`
  for supported MyLite-routed CTAS targets.
- Replacement of old rows, supported indexes, autoincrement state, requested
  engine metadata, and close/reopen visibility.
- Durable-sidecar gates around successful replacement.

## Non-Goals

- Failed OR REPLACE rollback that restores the old table when the replacement
  create or row population fails.
- `CREATE OR REPLACE TEMPORARY TABLE`, `IGNORE` / `REPLACE` CTAS, lock-table
  edge cases, views, foreign keys, partitions, triggers, or unsupported index
  classes.
- Physical compaction of pages made unreachable by the drop-then-create
  lifecycle.
- SQL transaction or savepoint semantics.

## Design

Use MariaDB's existing OR REPLACE paths:

1. Let MariaDB detect an existing target and drop it with
   `mysql_rm_table_no_locks()`.
2. Let the MyLite handler remove the old catalog record through
   `ha_mylite::delete_table()`.
3. Let the existing LIKE or CTAS creation path publish the replacement table
   definition through `ha_mylite::create()`.
4. Let normal MyLite row/index write paths populate replacement rows.
5. Keep failure rollback outside the support claim until MyLite has broader
   transactional DDL semantics.

No handler change is expected if the existing drop/create lifecycle works for
supported table shapes.

## Compatibility Impact

Successful OR REPLACE for representative routed LIKE and CTAS shapes moves from
planned to partial. The support claim is explicitly narrower than transactional
MariaDB semantics: a failed replacement may leave the old table dropped until a
separate DDL rollback slice exists.

## DDL Metadata Routing Impact

The old target catalog record is removed before publishing the new definition.
For no-engine LIKE replacement, MyLite preserves the source requested engine
metadata. For explicit CTAS replacement, the requested engine comes from the
replacement statement.

## Single-File And Embedded Lifecycle Impact

Successful replacements write only the primary `.mylite` file and permitted
MyLite rollback-journal state. No persistent `.frm`, `.ibd`, `.MYD`, `.MYI`,
`.MAI`, `.MAD`, Aria log, binlog, relay log, or plugin-owned durable table file
is introduced. Old table pages remain unreclaimed until compaction.

## Public API And File-Format Impact

No public `libmylite` API change and no storage file-format change.

## Storage-Engine Routing Impact

The covered statements execute through the MyLite handler for omitted/default
and `ENGINE=InnoDB` requests. Compatible-engine routing remains unchanged.

## Binary-Size And Dependency Impact

No dependency is added. Binary-size impact should be limited to storage-smoke
test code unless handler fixes are needed.

## Test And Verification Plan

- Add storage-engine smoke coverage that:
  - creates a source table and an old target table;
  - replaces the target with `CREATE OR REPLACE TABLE target LIKE source`;
  - verifies old rows are gone, source requested-engine metadata is preserved,
    autoincrement is reset, supported unique/index paths work, and reopen
    preserves the replacement;
  - replaces an existing CTAS target with
    `CREATE OR REPLACE TABLE target ENGINE=InnoDB AS SELECT ...`;
  - verifies old rows are gone, new CTAS rows and requested-engine metadata are
    visible, and close/reopen preserves the replacement;
  - verifies durable sidecar gates.
- Run targeted storage-smoke tests, routed DDL/DML and sidecar compatibility
  reports, format, tidy, diff, shell checks, and full preset gates.

## Acceptance Criteria

- Supported successful OR REPLACE LIKE and CTAS statements work through MyLite
  storage-engine smoke.
- Replaced targets expose only the new definition and rows before and after
  close/reopen.
- Catalog metadata reflects the replacement table, not the old target.
- Docs keep failed OR REPLACE rollback and broader variants marked planned.

## Implementation Status

Implemented in storage-engine smoke coverage:

- `CREATE OR REPLACE TABLE target LIKE source` replaces an old routed target,
  preserves the source requested-engine metadata, resets target rows and
  autoincrement state, and keeps supported unique and secondary indexes usable.
- `CREATE OR REPLACE TABLE target ENGINE=InnoDB AS SELECT ...` replaces an old
  CTAS target, copies representative source rows, preserves requested/effective
  engine metadata, and keeps supported unique and secondary indexes usable.
- Both replacement targets survive close/reopen without durable MariaDB
  sidecars.

## Risks And Unresolved Questions

- MariaDB's OR REPLACE flow is drop-then-create. Without DDL rollback support,
  failed replacements may remove the old table. This slice must not imply
  transactional replacement semantics.
- OR REPLACE interacts with locks, temporary tables, binary logging, and
  duplicate source/target checks. This slice covers only representative
  embedded, binlog-disabled, file-backed MyLite paths.
