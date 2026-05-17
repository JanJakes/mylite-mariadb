# Index Rename DDL

## Problem

MyLite already covers supported index creation, drop, and preservation across
table rename. This slice moved SQL-level `ALTER TABLE ... RENAME INDEX` from
unproven to partial support. Applications and schema migration tools use this
syntax to rename secondary indexes without changing table rows or key columns,
so MyLite needs coverage that the renamed key remains catalog-backed, usable
through index hints, and durable after close/reopen.

## Source Findings

MariaDB base: `mariadb-11.8.6`
(`9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`).

- `mariadb/sql/sql_yacc.yy` parses `ALTER TABLE ... RENAME INDEX` and
  `RENAME KEY` into `Alter_rename_key` entries and sets `ALTER_RENAME_INDEX`.
- `mariadb/sql/sql_table.cc` validates renamed keys, rewrites the in-memory key
  name, and includes the renamed key in the table rebuild metadata.
- `mariadb/sql/handler.cc` lists `ALTER_RENAME_INDEX` among ALTER flags that
  storage engines may receive during online or copy alter handling.
- MariaDB MTR coverage exists in `mariadb/mysql-test/main/ignored_index.test`
  and `mariadb/mysql-test/suite/innodb/t/innodb_rename_index.test`.

## Scope

- `ALTER TABLE ... RENAME INDEX ... TO ..., ALGORITHM=COPY` on supported
  MyLite-routed base-column and generated-column indexes.
- Renamed unique secondary index metadata, including generated-column unique
  indexes.
- Forced-index lookup through the new key name.
- Rejection of the old key name through `FORCE INDEX`.
- Duplicate-key enforcement after rename.
- Close/reopen discovery of the renamed key.

## Non-Goals

- Primary-key rename.
- Multi-rename conflict matrices.
- Instant, in-place, online, or lock-free index rename.
- Renaming unsupported FULLTEXT, SPATIAL, MySQL-style expression, hidden
  generated, generated primary-key, or oversized BLOB/TEXT indexes.
- SQL rollback or crash recovery for interrupted rename-index DDL.

## Design

Use MariaDB's normal copy ALTER path. The existing MyLite handler already
accepts supported rebuilt table shapes, publishes copied rows and index-entry
pages, and removes intermediate catalog definitions during copy rebuilds.
Renaming an index should therefore require no new storage page type: MariaDB
supplies a rebuilt table definition with the renamed key, and MyLite writes the
new table-definition blob plus rebuilt index-entry pages into the primary
`.mylite` file.

The storage-engine smoke test extends the standalone index DDL scenario:

1. Create a table and standalone unique index.
2. Rename that index with `ALGORITHM=COPY`.
3. Repeat the rename path for a generated-column unique index.
4. Verify `SHOW INDEX` exposes the new key name and not the old key name.
5. Verify `FORCE INDEX` works with the new key and fails with the old key.
6. Verify duplicate-key enforcement still uses the renamed unique index.
7. Close and reopen, then repeat representative new-key and old-key checks.

## Compatibility Impact

`ALTER TABLE ... RENAME INDEX` moves from planned to partial for supported
copy-rebuild MyLite-routed tables, including generated-column secondary and
unique indexes. It remains partial because online rename paths, primary-key
rename, conflict matrices, unsupported index classes, broader foreign-key
matrices, and transactional DDL rollback remain separate work. A follow-up
foreign-key slice covers referenced parent unique secondary-key rename for
MyLite's supported FK subset.

## Single-File And Embedded-Lifecycle Impact

Successful rename-index DDL keeps durable application state in the primary
`.mylite` file. Existing sidecar gates must continue rejecting persistent
MariaDB `.frm`, InnoDB, MyISAM, Aria, binlog, relay-log, and plugin-owned
sidecars.

## Build, Size, And Dependencies

No new dependency or production build-profile change is expected. The slice
adds compatibility coverage over existing MariaDB ALTER and MyLite copy-rebuild
paths.

## Test And Verification Plan

- Extend `mylite_embedded_storage_engine_test` in the storage-smoke preset.
- Run the focused storage-engine test.
- Run the routed DDL/DML and sidecar compatibility-harness reports.
- Run format, tidy, dev, embedded-dev, storage-smoke-dev, and diff checks.

## Acceptance Criteria

- `ALTER TABLE ... RENAME INDEX ... ALGORITHM=COPY` succeeds.
- The old index name disappears from `SHOW INDEX` and `FORCE INDEX`.
- The new index name works for forced-index reads before and after reopen.
- Duplicate-key checks still reject conflicting rows after rename.
- Docs, roadmap, and compatibility matrix describe the partial support.

## Implementation Status

Implemented in the storage-engine smoke test:

- `mylite_embedded_storage_engine_test` renames a standalone-created unique
  secondary index on an `ENGINE=InnoDB` MyLite-routed table with
  `ALGORITHM=COPY`.
- The test verifies `SHOW INDEX` old-name removal and new-name discovery,
  old-name `FORCE INDEX` rejection, new-name forced-index reads,
  duplicate-key rejection, generated-column unique-index rename coverage,
  close/reopen discovery, catalog table-count stability, and sidecar cleanup.
- A later FK-focused slice extends the same copy-rebuild path to referenced
  parent unique secondary-key rename, with FK metadata and row-check coverage.
- No production storage change was required because the current copy ALTER path
  already republishes the rebuilt table definition and index-entry pages.

## Risks And Open Questions

- This slice verifies one supported copy-rebuild path, not MariaDB's full
  rename-index conflict matrix.
- If a future storage format stores physical index names separately from
  table-definition metadata, rename-index DDL will need an explicit catalog
  migration path.
