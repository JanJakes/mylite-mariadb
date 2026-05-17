# Foreign-Key CTAS Targets

## Problem

MyLite supports `CREATE TABLE ... SELECT` for routed tables and supports a
bounded public foreign-key subset, but the roadmap still treats CTAS plus
foreign keys as unproven. This matters for migration and fixture-import flows
that create a constrained table and populate it from a deterministic query in
one statement.

This slice proves that explicit `CREATE TABLE ... SELECT` targets with
supported `RESTRICT` / `NO ACTION` foreign keys publish metadata, enforce
selected rows, roll back failed target creation, and survive close/reopen.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_insert.cc:select_create::create_table_from_items()` creates
  and opens the CTAS target through `mysql_create_table_no_lock()` before rows
  are inserted.
- `mariadb/sql/sql_insert.cc:select_create::store_values()` writes each
  selected row through the normal insert path, so MyLite target-row checks run
  through `ha_mylite::write_row()`.
- `mariadb/sql/sql_insert.cc:select_create::abort_result_set()` delegates to
  the insert abort path and drops the just-created CTAS target on error.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::create()` validates and
  stores MyLite FK metadata during table creation, using catalog-backed parent
  table metadata rather than native InnoDB dictionary state.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::write_row()` calls
  `mylite_check_child_foreign_keys()` for durable routed tables while
  `foreign_key_checks` is enabled.

MariaDB documents `CREATE TABLE ... SELECT` as a supported table-creation form
and documents foreign keys as storage-engine-owned referential constraints:

- <https://mariadb.com/docs/server/server-usage/tables/create-table>
- <https://mariadb.com/docs/server/architecture/server-constraints/foreign-key-constraints>

## Scope

- Explicit non-temporary CTAS targets with supported FK definitions.
- Same-file durable MyLite-routed parent and child tables.
- `ENGINE=InnoDB` target requests that resolve to the MyLite handler.
- Successful CTAS rows that satisfy the referenced parent key.
- Failed CTAS rows that violate the child FK and must remove the target table
  and FK metadata.
- Close/reopen table, row, `SHOW CREATE TABLE`, information-schema, and row
  enforcement checks.

## Non-Goals

- Cascades, `SET NULL`, `SET DEFAULT`, or deferrable checks.
- CTAS targets with partitions, temporary tables, volatile rows, unsupported
  index classes, or unsupported FK shapes.
- CTAS from views, routines, information schema, or server-only sources.
- Full transactional DDL beyond the existing statement checkpoint and CTAS
  abort/drop behavior.

## Design

Use the already implemented MariaDB CTAS path and MyLite FK path together:

1. `ha_mylite::create()` validates the explicit target FK definition and
   stores the table plus FK catalog metadata under the current statement
   checkpoint.
2. CTAS selected rows enter `ha_mylite::write_row()`, which performs existing
   duplicate, generated/CHECK, autoincrement, index, and child-FK checks.
3. A child-FK violation aborts the CTAS statement. MariaDB drops the just
   created target and MyLite's checkpoint restores the statement-start catalog
   view.
4. Successful CTAS targets reuse existing FK metadata hooks for
   `SHOW CREATE TABLE`, information schema, and parent/child row checks after
   close/reopen.

## Compatibility Impact

`CREATE TABLE ... SELECT` remains partial, but foreign-key-constrained target
tables move from unproven to covered for the current public MyLite FK subset.
This does not widen FK actions or claim full InnoDB parity.

## DDL Metadata Routing Impact

Successful CTAS publishes ordinary table metadata plus FK records in the
primary `.mylite` catalog. Failed FK-constrained CTAS must not leave the target
table visible through catalog discovery, `SHOW TABLES`, or FK metadata hooks.

## Single-File And Embedded Lifecycle

No new companion files or file-format fields are introduced. Table metadata,
FK metadata, rows, and index entries remain in the primary `.mylite` file and
use the existing rollback-journal/checkpoint lifecycle.

## Public API And File Format

No public C API or storage format change.

## Storage-Engine Routing Impact

The tested target requests `ENGINE=InnoDB`, which routes to MyLite and records
requested `InnoDB` with effective `MYLITE`.

## Test And Verification Plan

- Extend storage-engine smoke coverage for:
  - successful FK-constrained CTAS over existing parent rows;
  - FK metadata visibility in `SHOW CREATE TABLE` and
    `INFORMATION_SCHEMA.REFERENTIAL_CONSTRAINTS`;
  - child insert and parent update/delete enforcement after successful CTAS;
  - failed FK-constrained CTAS cleanup before and after catalog inspection;
  - close/reopen durability and enforcement.
- Run the storage-smoke embedded test target and `git diff --check`.

## Acceptance Criteria

- Supported FK-constrained CTAS succeeds and inserts selected rows.
- Failed FK-constrained CTAS leaves no target table or FK metadata.
- The FK remains visible and enforced after close/reopen.
- Docs and roadmap distinguish this from broader FK and CTAS support.

## Risks And Unresolved Questions

- CTAS abort still relies on the current checkpoint/drop behavior and can leave
  physically orphaned pages until compaction work exists.
- Broader CTAS duplicate-mode and unsupported-source matrices remain separate
  compatibility work.
