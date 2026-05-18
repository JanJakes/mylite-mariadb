# CREATE OR REPLACE Generated And CHECK Table

## Goal

Cover representative `CREATE OR REPLACE TABLE target (...)` replacement where
the replacement definition contains generated columns, generated-column indexes,
and named CHECK constraints. The replacement must remove the old routed table's
rows, indexes, generated metadata, and CHECK metadata, publish the new
definition, and preserve the new behavior across close/reopen.

## Non-Goals

- Do not change MariaDB's drop-then-create `OR REPLACE` semantics.
- Do not cover every generated-column or CHECK expression shape.
- Do not add full DDL transaction or savepoint semantics.
- Do not add FK, trigger, view, partition, lock-table, or unsupported-index
  `OR REPLACE` variants.
- Do not compact unreachable pages from the replaced table.
- Do not change the MyLite public API, file format, dependencies, or size
  profile.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/structs.h:591-621` defines
  `DDL_options_st::OPT_OR_REPLACE` and exposes `or_replace()`.
- `mariadb/sql/sql_table.cc:create_table_impl()` checks for an existing
  non-temporary table and, when `options.or_replace()` is set, calls
  `mysql_rm_table_no_locks()` before continuing with the replacement create.
- `mariadb/sql/sql_table.cc:create_table_impl()` calls
  `fix_constraints_names()` before table creation, so replacement CHECK names
  are normalized through MariaDB's normal DDL path.
- `mariadb/sql/unireg.cc:pack_vcols()` stores generated-column expressions,
  column CHECK expressions, and table CHECK expressions in the table-definition
  image.
- `mariadb/sql/table.cc:parse_vcol_defs()` restores generated-column and CHECK
  metadata from the stored table-definition image during reopen.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::delete_table()` maps the
  MariaDB drop step to `mylite_storage_drop_table()`.
- `mariadb/storage/mylite/ha_mylite.cc:ha_mylite::create()` stores the
  replacement MariaDB table-definition image through
  `mylite_storage_store_table_definition()`.

## Compatibility Impact

This narrows the remaining OR REPLACE edge-case gap for supported MyLite-routed
table metadata. MyLite already covers representative plain, LIKE, CTAS,
rollback, and FK-aware replacement paths; this slice verifies that replacement
of advanced generated/CHECK metadata uses the same durable catalog path without
leaking the old metadata after reopen.

## Design

Use MariaDB's existing `CREATE OR REPLACE TABLE` flow:

1. Create an old routed table with generated metadata, CHECK metadata, rows, and
   an index over the generated column.
2. Replace it with a new routed table containing a different generated column,
   a generated-column unique key, a CHECK constraint, and a secondary key.
3. Verify old rows, old columns, old indexes, and old CHECK behavior are no
   longer SQL-visible.
4. Verify the replacement generated values, generated-column index,
   CHECK enforcement, duplicate checks, metadata, and sidecar gates before and
   after close/reopen.

No production change is expected unless the test exposes stale metadata or a
missing statement-checkpoint/catalog publication hook.

## DDL Metadata Routing Impact

The replacement definition is stored as a new MariaDB table-definition image in
the MyLite catalog. Requested engine metadata should reflect the replacement
statement, and effective engine metadata remains `MYLITE`.

## Single-File And Embedded Lifecycle Impact

The statement may leave unreachable pages for the replaced table until
compaction exists, but all SQL-visible durable state must remain in the primary
`.mylite` file. The replacement must not create persistent `.frm`, `.ibd`,
`.MYD`, `.MYI`, `.MAI`, `.MAD`, `aria_log.*`, binlog, relay-log, or
plugin-owned table files.

## Public API And File Format Impact

No public `libmylite` API or storage file-format change is required.

## Storage-Engine Routing Impact

The old table uses an explicit routed engine and the replacement uses
`ENGINE=InnoDB`, which routes to MyLite while preserving the requested engine
name.

## Build, Size, And Dependencies

No dependency or intended size-profile change is introduced. The expected
change is storage-smoke test and documentation coverage.

## Test Plan

- Add storage-engine smoke coverage for generated/CHECK metadata in plain
  OR REPLACE.
- Update compatibility, storage architecture, roadmap, and OR REPLACE docs.
- Build `mylite_embedded_storage_engine_test`.
- Run the focused storage-engine smoke binary.
- Run `ctest --preset storage-smoke-dev`, `ctest --preset dev`, and
  `git diff --check`.

## Acceptance Criteria

- Plain `CREATE OR REPLACE TABLE ... ENGINE=InnoDB` can replace an existing
  generated/CHECK routed table with a new generated/CHECK definition.
- Old rows, columns, generated indexes, and CHECK behavior are not SQL-visible
  after replacement.
- New generated values, generated-column indexes, unique checks, and CHECK
  constraints work before and after close/reopen.
- Requested/effective engine metadata and durable sidecar gates remain correct.

## Risks And Unresolved Questions

- MariaDB's plain `OR REPLACE` remains drop-then-create. This slice does not
  imply full transactional DDL beyond the covered statement-checkpoint behavior.
- Broader OR REPLACE matrices for lock-table interactions, unsupported object
  classes, and exhaustive generated/CHECK expressions remain planned.
