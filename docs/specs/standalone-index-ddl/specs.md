# Standalone Index DDL

## Problem

MyLite supports primary, unique, and secondary indexes declared in
`CREATE TABLE` and added through copy `ALTER TABLE`. This slice moved
standalone `CREATE INDEX` and `DROP INDEX` from planned to partial support.
Real MySQL/MariaDB schemas often use the standalone forms, so MyLite proves
they route through the same catalog and index-entry lifecycle.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_lex.h` parses standalone `CREATE INDEX` into
  `SQLCOM_CREATE_INDEX` with `ALTER_ADD_INDEX` in `LEX::alter_info`.
- `mariadb/sql/sql_yacc.yy` parses standalone `DROP INDEX` into
  `SQLCOM_DROP_INDEX` with `ALTER_DROP_INDEX` in `LEX::alter_info`.
- `mariadb/sql/sql_parse.cc` handles `SQLCOM_CREATE_INDEX` and
  `SQLCOM_DROP_INDEX` by calling `mysql_alter_table()` with prepared
  `Alter_info`, so MyLite should use the existing copy-ALTER handler path
  rather than a separate executor.
- `mariadb/sql/sql_table.cc:check_engine()` documents that enforced storage
  engine substitution should not be applied to standalone `CREATE INDEX`.
- `mariadb/mysql-test/main/vector.test` covers non-vector standalone
  `CREATE INDEX ... ALGORITHM=COPY` and `DROP INDEX` as ALTER-equivalent DDL.

## Scope

- Standalone `CREATE INDEX ... ALGORITHM=COPY` over supported MyLite-routed
  table shapes.
- Standalone `CREATE UNIQUE INDEX ... ALGORITHM=COPY` duplicate checks after
  the rebuilt index exists.
- Standalone `DROP INDEX` over supported MyLite-routed table shapes.
- Standalone `CREATE INDEX` / `DROP INDEX` on supported generated-column
  secondary indexes.
- Catalog metadata, row visibility, forced-index lookup, close/reopen,
  catalog-only reopened default-algorithm index DDL, and durable-sidecar gates.

## Non-Goals

- In-place, instant, or online standalone index DDL.
- Unsupported index classes: FULLTEXT, SPATIAL, MySQL-style expression keys,
  hidden generated keys, hash keys, long-unique hash keys, foreign keys, or
  oversized/full BLOB/TEXT keys. Generated primary keys follow MariaDB's
  SQL-layer rejection policy.
- Transaction rollback, statement rollback, or crash recovery for failed or
  interrupted standalone index DDL.
- A separate MyLite parser or executor for standalone index DDL.

## Design

Use MariaDB's existing standalone-index-to-ALTER path. MyLite's handler already
implements copy rebuilds, supported index definition validation, index-entry
publication, catalog rename/drop cleanup, and sidecar gates for the copy
`ALTER TABLE` path. Standalone index DDL should therefore exercise the same
flow:

1. MariaDB parses the standalone statement into `Alter_info`.
2. MariaDB calls `mysql_alter_table()`.
3. MyLite accepts or rejects the rebuilt table shape in `ha_mylite::create()`.
4. MyLite preserves the source table's requested engine metadata for
   standalone index rebuilds, because the statement does not carry an explicit
   `ENGINE=...` clause.
5. MariaDB copies rows through the handler, then renames/drops intermediate
   catalog records.
6. MyLite serves the resulting key through the existing index cursor path.

No new public API, storage page type, or compatibility harness group is
required. The existing `routed-ddl-dml`, `storage-engine`, and `sidecar`
groups cover this SQL surface.

## Compatibility Impact

Standalone `CREATE INDEX` and `DROP INDEX` move from planned to partial for
supported copy-rebuild table shapes, including generated-column secondary
indexes and representative default-algorithm standalone index DDL after
catalog-only reopen. They remain partial because online DDL, unsupported index
classes, SQL rollback, foreign keys, and broader crash recovery still need
separate slices.

## Single-File, Storage, And Embedded Lifecycle Impact

Successful standalone index DDL appends rebuilt table definitions, rows, and
index-entry pages to the primary `.mylite` file through the existing copy
rebuild path. Old pages remain orphaned until free-space reclamation exists.
No durable `.frm`, `.ibd`, `.MYD`, `.MYI`, `.MAI`, `.MAD`, or Aria log
sidecars are allowed.

## Test Plan

1. Extend storage-engine smoke coverage for:
   - standalone non-unique `CREATE INDEX ... ALGORITHM=COPY`;
   - standalone `CREATE UNIQUE INDEX ... ALGORITHM=COPY`;
   - duplicate-key enforcement after standalone unique index creation;
   - forced-index lookup through the new standalone indexes;
   - standalone `DROP INDEX` removing metadata and preserving row visibility;
   - generated-column standalone index creation and drop through the same
     copy-rebuild path;
   - close/reopen visibility, reopened default-algorithm standalone
     `CREATE INDEX` / `DROP INDEX`, and no durable sidecars.
2. Run storage-smoke routed DDL/DML and sidecar groups.
3. Run normal dev, embedded-dev, storage-smoke-dev, format, tidy, diff, shell,
   and size checks.

## Acceptance Criteria

- Supported standalone `CREATE INDEX` and `DROP INDEX` complete through
  MariaDB SQL execution.
- Added indexes are visible to forced-index queries and unique duplicate
  checks.
- Generated-column indexes use the same standalone create/drop lifecycle as
  supported base-column indexes.
- Dropped indexes are no longer visible to `SHOW INDEX`, while table rows
  remain readable.
- Catalog table count remains stable after intermediate copy-rebuild cleanup.
- Docs and compatibility matrix mark standalone index DDL as partial, not
  planned.

## Risks And Open Questions

- Standalone `DROP INDEX` does not accept an explicit `ALGORITHM=COPY` in the
  same form as `CREATE INDEX`, so the test validates the default MariaDB path.
- This slice relies on current copy-ALTER atomic publication limits. SQL
  rollback and crash recovery for interrupted rebuilds remain separate work.
