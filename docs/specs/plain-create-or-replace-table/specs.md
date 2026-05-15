# Plain CREATE OR REPLACE TABLE

## Goal

Cover successful plain `CREATE OR REPLACE TABLE target (...)` over an existing
MyLite-routed base table. The replacement must remove the old SQL-visible
definition, rows, indexes, and autoincrement state, publish the new routed
definition, preserve requested/effective engine metadata, and survive
close/reopen without durable MariaDB sidecars.

## Non-Goals

- `CREATE OR REPLACE TABLE ... LIKE` and CTAS coverage, already handled by
  earlier slices.
- Failed replacement rollback, already covered for representative self-LIKE,
  unsupported-definition, and duplicate-key CTAS failures.
- Temporary-table, lock-table, binlog, partition, foreign-key, trigger, view,
  and unsupported-index `OR REPLACE` variants.
- SQL transaction or savepoint semantics for replacement DDL.
- Physical compaction of row, index, and definition pages made unreachable by
  the replacement.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- MariaDB documentation for `CREATE TABLE` documents `CREATE [OR REPLACE]
  TABLE` syntax, describes `OR REPLACE` as dropping an existing table before
  creating the new one, and notes that a failed replacement create can leave
  the original table absent:
  <https://mariadb.com/kb/en/create-table/>.
- `mariadb/sql/structs.h:591-621` defines
  `DDL_options_st::OPT_OR_REPLACE`.
- `mariadb/sql/sql_parse.cc:3130-3135` applies the extra DROP privilege check
  for non-temporary `CREATE OR REPLACE`.
- `mariadb/sql/sql_table.cc:4772-4822` handles `OR REPLACE` over an existing
  base table by calling `mysql_rm_table_no_locks()` before the new create and
  setting `table_was_deleted`.
- `mariadb/sql/sql_table.cc:5001-5063` routes plain table creation through
  `create_table_impl()` after the optional replacement drop.
- `mariadb/sql/sql_table.cc:5240-5293` calls
  `mysql_create_table_no_lock()` for ordinary `CREATE TABLE` execution.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::delete_table()` maps the
  MariaDB drop step to `mylite_storage_drop_table()`, and
  `ha_mylite::create()` publishes the replacement definition with
  `mylite_storage_store_table_definition()`.

## Compatibility Impact

`CREATE OR REPLACE TABLE` moves from representative LIKE/CTAS-only success
coverage to representative plain, LIKE, and CTAS success coverage. The support
claim remains partial: broader error matrices, lock-table interactions,
temporary-table edge cases, and full SQL transaction semantics remain planned.

## Design

Use MariaDB's existing plain `OR REPLACE` flow without a MyLite-specific SQL
rewrite:

1. MariaDB detects the existing routed base table.
2. MariaDB drops it through `mysql_rm_table_no_locks()`.
3. The MyLite handler removes the old catalog record.
4. MariaDB creates the replacement table through the normal plain create path.
5. The MyLite handler stores the new catalog definition, requested engine, and
   effective `MYLITE` engine.
6. Normal MyLite row, index, and autoincrement paths handle subsequent writes
   to the replacement table.

No handler change is expected unless the test exposes a missing catalog,
statement-checkpoint, or sidecar lifecycle hook.

## File Lifecycle

The statement may use MyLite's rollback journal during publication. It must not
leave persistent `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, `aria_log.*`,
binlog, relay-log, or plugin-owned table files. Old pages become unreachable
but are not reclaimed until compaction exists.

## Embedded Lifecycle And API

No public `libmylite` API changes. Existing direct SQL execution and diagnostics
surface the MariaDB statement result.

## Build, Size, And Dependencies

No new dependency or build-profile change. Binary-size impact is limited to test
code unless a handler fix is required.

## Test Plan

- Extend storage-engine smoke coverage to:
  - create an old routed target with rows, a unique key, a secondary prefix
    index, and advanced autoincrement state;
  - replace it with plain `CREATE OR REPLACE TABLE ... ENGINE=InnoDB`;
  - verify old rows are gone, old indexes are no longer usable, new requested
    engine metadata is stored, and the catalog table count is unchanged;
  - insert replacement rows, verify autoincrement reset, unique/index behavior,
    and duplicate rejection;
  - close/reopen and verify the replacement metadata, rows, indexes, and
    sidecar gates persist.
- Run the focused storage-smoke test, routed DDL/DML and sidecar compatibility
  reports, format checks, shell checks, tidy, and the relevant CTest presets.

## Acceptance Criteria

- Plain `CREATE OR REPLACE TABLE target (...) ENGINE=InnoDB` replaces an
  existing routed MyLite table without persistent MariaDB sidecars.
- The old target definition, rows, indexes, and autoincrement state are not
  SQL-visible after replacement.
- The replacement table remains usable and discoverable after close/reopen.
- Compatibility, storage architecture, harness, roadmap, and related specs name
  plain `OR REPLACE` as covered while keeping broader edge cases planned.

## Risks And Open Questions

- MariaDB's plain `OR REPLACE` is drop-then-create. MyLite's representative
  rollback coverage does not imply full SQL transaction semantics.
- Catalog page compaction remains planned, so old unreachable pages remain in
  the primary file.
- Lock-table behavior is intentionally outside this embedded slice because
  MyLite rejects SQL locking surfaces until lock semantics are designed.
