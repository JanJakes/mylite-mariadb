# Generated BLOB/TEXT Prefix Index DDL

## Problem

MyLite now supports bounded generated BLOB/TEXT prefix indexes declared in
initial table DDL. The remaining adjacent gap is standalone index DDL over the
same generated BLOB/TEXT columns. MariaDB routes `CREATE INDEX` and
`DROP INDEX` through copy-rebuild table DDL, so this should reuse the existing
generated-value, BLOB/TEXT-prefix key image, and catalog publication paths.

## Source Findings

- MariaDB base: `mariadb-11.8.6` /
  `9bfea48ce1214cc4470f6f6f8a4e30352cef84e7`.
- `mariadb/sql/sql_table.cc`: standalone `CREATE INDEX` / `DROP INDEX` is
  represented as table ALTER work and copy rebuilds when needed.
- `mariadb/sql/sql_table.cc`: copy ALTER rebuilds call
  `TABLE::update_virtual_fields()` while copying rows that need generated
  values.
- `mariadb/sql/table.cc`: `TABLE::update_keypart_vcol_info()` reconnects
  generated-column metadata to key parts after table metadata is reopened.
- `mariadb/sql/key.cc`: `key_copy()` builds generated BLOB/TEXT prefix key
  tuples through MariaDB field key-image logic.
- `mariadb/storage/mylite/ha_mylite.cc`: MyLite publishes rebuilt table
  definitions through the catalog and stores opaque index-entry key bytes.

## Scope

- Standalone `CREATE INDEX ... ALGORITHM=COPY` and
  `CREATE UNIQUE INDEX ... ALGORITHM=COPY` over generated BLOB/TEXT prefix
  columns on MyLite-routed tables.
- Standalone `DROP INDEX` for generated BLOB/TEXT prefix indexes.
- Forced-index reads, duplicate-prefix checks, close/reopen visibility,
  catalog metadata, and sidecar gates.

## Non-Goals

- Online, instant, in-place, or lock-free index DDL.
- Full or oversized generated BLOB/TEXT key payloads.
- Expression/hidden indexes, FULLTEXT, SPATIAL, hash, vector, or foreign-key
  indexes. Generated primary keys follow MariaDB's SQL-layer rejection policy.
- SQL rollback, savepoints, crash recovery beyond current statement
  checkpointing, or broad generated expression matrices.

## Design

The standalone DDL path should compose existing pieces:

1. MariaDB parses standalone index DDL as ALTER work.
2. Copy rebuild creates the new table definition and computes generated values
   while copying existing rows.
3. MyLite accepts bounded generated BLOB/TEXT prefix key parts through the
   existing key-shape predicate.
4. MyLite persists the rebuilt table definition and generated prefix index
   entries in the primary `.mylite` file.
5. `DROP INDEX` republishes the rebuilt table definition without the generated
   prefix index.

## Compatibility Impact

Generated BLOB/TEXT prefix support expands from initial DDL to standalone
copy-rebuild index DDL. Online DDL, full/oversized key payloads,
expression/hidden generated indexes, and transaction-aware DDL rollback remain
planned; generated primary keys follow MariaDB's SQL-layer rejection policy.

## DDL Metadata Routing Impact

Successful standalone generated BLOB/TEXT prefix index DDL republishes MyLite
catalog table metadata through the copy-rebuild path and must remain
discoverable after close/reopen without durable MariaDB metadata sidecars.

## Single-File And Embedded-Lifecycle Impact

Generated prefix index entries and rebuilt table definitions remain in the
primary `.mylite` file. No durable `.frm`, engine sidecar, or runtime schema
directory should be required after close/reopen.

## Public API, File-Format, Size, And Dependency Impact

No public API, file-format, dependency, or embedded profile change is expected.
The slice adds compatibility coverage and documentation unless the existing DDL
path needs a fix.

## Test And Verification Plan

- Extend storage-engine smoke standalone index coverage with a routed table
  that has generated BLOB/TEXT columns and no initial generated indexes.
- Add a generated stored `TEXT` prefix index with standalone `CREATE INDEX`.
- Add a generated virtual `TEXT` unique prefix index with standalone
  `CREATE UNIQUE INDEX`.
- Add a generated stored `BLOB` prefix index with standalone `CREATE INDEX`.
- Verify forced-index reads and duplicate-prefix checks.
- Drop one generated prefix index and verify rows remain readable.
- Verify catalog metadata and remaining generated prefix indexes after
  close/reopen.
- Run generated-column, unsupported-index, routed DDL/DML, sidecar, format,
  tidy, preset, and diff checks.

## Acceptance Criteria

- Standalone copy-rebuild index DDL can add bounded generated BLOB/TEXT prefix
  indexes.
- Standalone drop-index DDL can remove a generated BLOB/TEXT prefix index
  without losing rows.
- Generated BLOB/TEXT prefix reads and duplicate checks survive close/reopen.
- Compatibility docs distinguish this support from online DDL and full or
  oversized generated BLOB/TEXT key payloads.

## Risks And Open Questions

- This still relies on append-only row/index publication and current statement
  checkpointing.
- Broader generated expressions and default-algorithm standalone DDL need
  separate compatibility matrices.
